#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "CpuX64.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Lumen::Loader {
// Reuse the production FEX reader seam without adding a runtime test API.
extern thread_local bool (*g_host_call_guest_memory_read)(Cpu::X64State*, uint64_t, size_t, void*, size_t);
}

namespace {
namespace Video = Lumen::Libs::VideoOut;
namespace Memory = Lumen::Core::VirtualMemory;
constexpr int INVALID_VALUE = -2144796671;
constexpr int INVALID_ADDRESS = -2144796670;
constexpr int INVALID_HANDLE = -2144796661;
constexpr int UNSUPPORTED = -2144796645;
constexpr int UNSUPPORTED_OPERATION = -2144796644;
static_assert(INVALID_VALUE == Video::VIDEO_OUT_ERROR_INVALID_VALUE);
static_assert(INVALID_ADDRESS == Video::VIDEO_OUT_ERROR_INVALID_ADDRESS);
static_assert(INVALID_HANDLE == Video::VIDEO_OUT_ERROR_INVALID_HANDLE);
static_assert(UNSUPPORTED == Video::VIDEO_OUT_ERROR_PORT_UNSUPPORTED_FUNCTION);
static_assert(UNSUPPORTED_OPERATION == Video::VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION);
static_assert(sizeof(float) == 4);

// Original direct-wrapper tests for the local policies in
// VIDEOOUT_FOLLOWUP_INTERFACE.md and VIDEOOUT_ADJUST_COLOR_INTERFACE.md.
// The registered fixed-GP marshaler is also exercised below. These do not
// test display effects or execute FEX, which has its own execution-level gate.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "VideoOutGammaTest: %s\n", message);
	return condition;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Memory::Alloc(0, size, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(m_address + offset); }
	uint64_t Address() const { return m_address; }
	bool Protect(size_t offset, size_t size, Memory::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Memory::Protect(m_address + offset, size, mode);
	}
	bool Free()
	{
		if (!Valid() || !Memory::Free(m_address)) return false;
		m_address = 0;
		return true;
	}
private:
	size_t m_size;
	uint64_t m_address;
};

std::vector<uint8_t> Pattern(size_t size)
{
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 29 + 7);
	return bytes;
}

bool ValuesAndPreservation(size_t page_size)
{
	Mapping mapping(page_size);
	if (!Check(mapping.Valid(), "value fixture allocation failed")) return false;
	const auto original = Pattern(page_size);
	const std::array<float, 6> accepted {
	    0.1f, std::nextafter(0.1f, 2.0f), 0.3f, 1.0f, std::nextafter(2.0f, 0.1f), 2.0f,
	};
	for (size_t offset: {size_t(16), size_t(19)})
		for (float gamma: accepted)
		{
			std::memcpy(mapping.Data(), original.data(), original.size());
			auto expected = original;
			std::memcpy(expected.data() + offset, &gamma, sizeof(gamma));
			if (!Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(offset), gamma) == 0,
			           "valid gamma or unaligned destination was rejected") ||
			    !Check(std::memcmp(mapping.Data(), expected.data(), expected.size()) == 0,
			           "gamma bits, reserved cells or surrounding sentinels changed incorrectly")) return false;
		}
	const std::array<float, 9> rejected {
	    std::nextafter(0.1f, 0.0f), std::nextafter(2.0f, std::numeric_limits<float>::infinity()),
	    0.0f, -0.0f, -1.0f, 3.0f, std::numeric_limits<float>::infinity(),
	    -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
	};
	std::memcpy(mapping.Data(), original.data(), original.size());
	for (float gamma: rejected)
		if (!Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(19), gamma) == INVALID_VALUE &&
		           Video::VideoOutColorSettingsSetGamma(nullptr, gamma) == INVALID_VALUE,
		           "range/nonfinite error or its precedence over pointer validation was wrong") ||
		    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0,
		           "rejected gamma changed the record")) return false;
	return true;
}

bool ExactBindingFor(Lumen::Loader::SymbolDatabase* symbols, const char32_t* nid,
                     const char32_t* missing_nid, uint64_t target,
                     Lumen::Loader::HostCallScalarType second_argument)
{
	using namespace Lumen::Loader;
	SymbolResolve request {nid, U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	const auto* record = symbols->Find(request);
	if (!Check(record != nullptr && record->vaddr == target, "exact color NID/version did not bind")) return false;
	const auto* scalar = symbols->HostCallScalarMetadataFor(target);
	if (!Check(scalar != nullptr && scalar->arity == 2 && scalar->invoker != nullptr &&
	           scalar->arguments[0] == HostCallScalarType::Gp && scalar->arguments[1] == second_argument &&
	           scalar->result == HostCallScalarType::Gp, "color export lacks its fixed scalar signature")) return false;
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = request;
		switch (mismatch)
		{
			case 0: other.name = missing_nid; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(symbols->Find(other) == nullptr, "color export bound through a mismatched identity")) return false;
	}
	return true;
}

bool FixedGpBindings(const Lumen::Loader::SymbolDatabase& symbols)
{
	using namespace Lumen::Loader;
	struct Export { const char32_t* nid; uint64_t target; uint32_t arity; HostCallScalarType result; };
	const Export exports[] {
	    {U"Up36PTk687E", reinterpret_cast<uint64_t>(&Video::VideoOutOpen), 4, HostCallScalarType::Gp},
	    {U"uquVH4-Du78", reinterpret_cast<uint64_t>(&Video::VideoOutClose), 1, HostCallScalarType::Gp},
	    {U"kGVLc3htQE8", reinterpret_cast<uint64_t>(&Video::VideoOutGetDeviceCapabilityInfo), 3, HostCallScalarType::Gp},
	    {U"zgXifHT9ErY", reinterpret_cast<uint64_t>(&Video::VideoOutIsFlipPending), 1, HostCallScalarType::Gp},
	    {U"N1bEoJ4SRw4", reinterpret_cast<uint64_t>(&Video::VideoOutConfigureOutputMode), 6, HostCallScalarType::Gp},
	    {U"6kPnj51T62Y", reinterpret_cast<uint64_t>(&Video::VideoOutGetResolutionStatus), 2, HostCallScalarType::Gp},
	    {U"i6-sR91Wt-4", reinterpret_cast<uint64_t>(&Video::VideoOutSetBufferAttribute), 7, HostCallScalarType::Void},
	    {U"CBiu4mCE1DA", reinterpret_cast<uint64_t>(&Video::VideoOutSetFlipRate), 2, HostCallScalarType::Gp},
	    {U"HXzjK9yI30k", reinterpret_cast<uint64_t>(&Video::VideoOutAddFlipEvent), 3, HostCallScalarType::Gp},
	    {U"Xru92wHJRmg", reinterpret_cast<uint64_t>(&Video::VideoOutAddVblankEvent), 3, HostCallScalarType::Gp},
	    {U"w3BY+tAEiQY", reinterpret_cast<uint64_t>(&Video::VideoOutRegisterBuffers), 5, HostCallScalarType::Gp},
	    {U"U46NwOiJpys", reinterpret_cast<uint64_t>(&Video::VideoOutSubmitFlip), 4, HostCallScalarType::Gp},
	    {U"SbU3dwp80lQ", reinterpret_cast<uint64_t>(&Video::VideoOutGetFlipStatus), 2, HostCallScalarType::Gp},
	    {U"1FZBKy8HeNU", reinterpret_cast<uint64_t>(&Video::VideoOutGetVblankStatus), 2, HostCallScalarType::Gp},
	    {U"MTxxrOCeSig", reinterpret_cast<uint64_t>(&Video::VideoOutSetWindowModeMargins), 3, HostCallScalarType::Gp},
	};
	for (const auto& entry: exports)
	{
		const auto* symbol = symbols.Find({entry.nid, U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func});
		const auto* scalar = symbols.HostCallScalarMetadataFor(entry.target);
		if (!Check(symbol != nullptr && symbol->vaddr == entry.target && scalar != nullptr &&
		           scalar->arity == entry.arity && scalar->result == entry.result && scalar->invoker != nullptr,
		           "VideoOut export lacks its exact fixed scalar binding")) return false;
		for (uint32_t argument = 0; argument != scalar->arity; ++argument)
			if (!Check(scalar->arguments[argument] == HostCallScalarType::Gp,
			           "fixed GP VideoOut argument changed class")) return false;
	}
	return true;
}

bool ExactBinding()
{
	using namespace Lumen::Loader;
	SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libVideoOut_1", &symbols), "VideoOut binding initialization failed") ||
	    !FixedGpBindings(symbols) ||
	    !ExactBindingFor(&symbols, U"DYhhWbJSeRg", U"DYhhWbJSeRh",
	                     reinterpret_cast<uint64_t>(&Video::VideoOutColorSettingsSetGamma), HostCallScalarType::Float32) ||
	    !ExactBindingFor(&symbols, U"pv9CI5VC+R0", U"pv9CI5VC+R1",
	                     reinterpret_cast<uint64_t>(&Video::VideoOutAdjustColor), HostCallScalarType::Gp)) return false;
	SymbolResolve open {U"Up36PTk687E", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	const auto* open_record = symbols.Find(open);
	if (!Check(open_record != nullptr &&
	           open_record->vaddr == reinterpret_cast<uint64_t>(&Video::VideoOutOpen),
	           "exact VideoOutOpen NID/version did not bind")) return false;
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = open;
		switch (mismatch)
		{
			case 0: other.name = U"Up36PTk687F"; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(symbols.Find(other) == nullptr, "VideoOutOpen bound through a mismatched identity")) return false;
	}
	SymbolResolve capability {U"kGVLc3htQE8", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	const auto* capability_record = symbols.Find(capability);
	if (!Check(capability_record != nullptr &&
	           capability_record->vaddr == reinterpret_cast<uint64_t>(&Video::VideoOutGetDeviceCapabilityInfo),
	           "exact VideoOut capability NID/version did not bind")) return false;
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = capability;
		switch (mismatch)
		{
			case 0: other.name = U"kGVLc3htQE9"; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(symbols.Find(other) == nullptr, "VideoOut capability bound through a mismatched identity")) return false;
	}
	SymbolResolve pending {U"zgXifHT9ErY", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	const auto* pending_record = symbols.Find(pending);
	if (!Check(pending_record != nullptr &&
	           pending_record->vaddr == reinterpret_cast<uint64_t>(&Video::VideoOutIsFlipPending),
	           "exact VideoOut pending NID/version did not bind")) return false;
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = pending;
		switch (mismatch)
		{
			case 0: other.name = U"zgXifHT9ErZ"; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(symbols.Find(other) == nullptr, "VideoOut pending bound through a mismatched identity")) return false;
	}
	const SymbolResolve configure {U"N1bEoJ4SRw4", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = configure;
		switch (mismatch)
		{
			case 0: other.name = U"N1bEoJ4SRw5"; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(symbols.Find(other) == nullptr, "VideoOut configuration bound through a mismatched identity")) return false;
	}
	return Check(symbols.FindByStdName(U"sceVideoOutOpen") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutGetDeviceCapabilityInfo_") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutIsFlipPending") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutConfigureOutputMode_") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutConfigureOutputMode") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutColorSettingsSetGamma_") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutColorSettingsSetGamma") == nullptr &&
	             symbols.FindByStdName(U"sceVideoOutAdjustColor") == nullptr,
	             "unqualified VideoOut standard-name fallback was added");
}

thread_local unsigned g_stack_read_attempts = 0;

class RejectStackReads
{
public:
	RejectStackReads(): m_previous(Lumen::Loader::g_host_call_guest_memory_read), m_previous_attempts(g_stack_read_attempts)
	{
		g_stack_read_attempts = 0;
		Lumen::Loader::g_host_call_guest_memory_read = [](Lumen::Cpu::X64State* state, uint64_t base,
		                                               size_t offset, void*, size_t size)
		{
			++g_stack_read_attempts;
			state->memory_fault_address = base <= UINT64_MAX - offset ? base + offset : base;
			state->memory_fault_size = static_cast<uint32_t>(size);
			state->memory_fault_write = false;
			return false;
		};
	}
	~RejectStackReads()
	{
		Lumen::Loader::g_host_call_guest_memory_read = m_previous;
		g_stack_read_attempts = m_previous_attempts;
	}
	RejectStackReads(const RejectStackReads&) = delete;
	RejectStackReads& operator=(const RejectStackReads&) = delete;
private:
	decltype(Lumen::Loader::g_host_call_guest_memory_read) m_previous;
	unsigned m_previous_attempts;
};

bool RegisteredGpStackBoundary(size_t page_size)
{
	using namespace Lumen::Loader;
	Mapping stack(page_size * 2);
	if (!Check(stack.Valid() && stack.Protect(page_size, page_size, Memory::Mode::NoAccess),
	           "registered ABI stack guard allocation failed")) return false;
	const uint64_t stack_pointer = stack.Address() + page_size - sizeof(uint64_t);
	constexpr uint64_t return_address = UINT64_C(0x123456789abcdef0);
	constexpr uint64_t result_sentinel = UINT64_C(0xfedcba9876543210);
	std::memcpy(stack.Data(page_size - sizeof(uint64_t)), &return_address, sizeof(return_address));
	SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libVideoOut_1", &symbols), "registered ABI lookup initialization failed")) return false;
	symbols.SealHostCallRegistry();
	RejectStackReads reader;
	// A negative control proves that the installed reader detects the legacy
	// bridge's first unnecessary overflow read at the inaccessible RSP + 8.
	Lumen::Cpu::X64State legacy {Lumen::Cpu::X64State::HostCallOnly {}};
	legacy.gpr[0] = result_sentinel;
	legacy.gpr[4] = stack_pointer;
	legacy.gpr[7] = UINT32_MAX;
	if (!Check(InvokeHostCall(nullptr, reinterpret_cast<uint64_t>(&Video::VideoOutIsFlipPending), &legacy) &&
	           g_stack_read_attempts == 1 && legacy.memory_fault_address == stack_pointer + 8 &&
	           legacy.memory_fault_size == 8 && legacy.gpr[0] == result_sentinel,
	           "legacy control did not expose the unnecessary stack read")) return false;
	g_stack_read_attempts = 0;
	const auto invoke = [&](const char32_t* nid, std::array<uint64_t, 4> arguments, int expected,
	                        std::array<uint64_t, 2> tail = {UINT64_C(0x9876543212345678), UINT64_C(0x8765432123456789)})
	{
		const auto* symbol = symbols.Find({nid, U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func});
		if (!Check(symbol != nullptr, "registered ABI export was not found")) return false;
		Lumen::Cpu::X64State state {Lumen::Cpu::X64State::HostCallOnly {}};
		state.gpr[0] = result_sentinel;
		state.gpr[4] = stack_pointer;
		state.gpr[7] = arguments[0];
		state.gpr[6] = arguments[1];
		state.gpr[2] = arguments[2];
		state.gpr[1] = arguments[3];
		state.gpr[8] = tail[0];
		state.gpr[9] = tail[1];
		uint64_t before[16];
		std::memcpy(before, state.gpr, sizeof(before));
		if (!Check(InvokeHostCall(&symbols, symbol->vaddr, &state) && g_stack_read_attempts == 0 &&
		           state.memory_fault_size == 0 && state.gpr[0] == static_cast<uint32_t>(expected),
		           "registered GP call read stack arguments or changed the int32 result")) return false;
		return Check(std::memcmp(before + 1, state.gpr + 1, sizeof(before) - sizeof(before[0])) == 0 &&
		             std::memcmp(stack.Data(page_size - sizeof(uint64_t)), &return_address, sizeof(return_address)) == 0,
		             "registered GP marshaler changed arguments or the return-address slot");
	};
	const uint64_t inaccessible = stack.Address() + page_size;
	const std::array<uint64_t, 4> open {
	    UINT64_C(0xaabbccdd000000ff), UINT64_C(0xbbccddee00000000), UINT64_C(0xccddeeff00000000), 0,
	};
	auto opaque_open = open;
	opaque_open[3] = inaccessible;
	if (!invoke(U"Up36PTk687E", opaque_open, UNSUPPORTED_OPERATION)) return false;
	// A pointer with only high bits set must remain non-null; the three int
	// arguments above intentionally carry unrelated upper bits.
	opaque_open[3] = UINT64_C(1) << 32;
	if (!invoke(U"Up36PTk687E", opaque_open, UNSUPPORTED_OPERATION) ||
	    !invoke(U"Up36PTk687E", open, 1) ||
	    !invoke(U"Up36PTk687E", open, Video::VIDEO_OUT_ERROR_RESOURCE_BUSY) ||
	    !invoke(U"kGVLc3htQE8", {1, inaccessible, UINT64_MAX, 0}, UNSUPPORTED_OPERATION) ||
	    !invoke(U"kGVLc3htQE8", {UINT64_C(0x12345678ffffffff), inaccessible, UINT64_MAX, 0}, INVALID_HANDLE) ||
	    !invoke(U"zgXifHT9ErY", {1, 0, 0, 0}, UNSUPPORTED_OPERATION) ||
	    !invoke(U"zgXifHT9ErY", {UINT64_C(0x12345678ffffffff), 0, 0, 0}, INVALID_HANDLE) ||
	    !invoke(U"N1bEoJ4SRw4", {UINT64_C(0x1234567800000001), UINT64_C(0xaabbccdd00000000), inaccessible, UINT64_MAX},
	            UNSUPPORTED_OPERATION, {UINT64_C(0x9988776600000005), UINT64_C(0x88776655ffffffff)}) ||
	    !invoke(U"N1bEoJ4SRw4", {UINT64_C(0x12345678ffffffff), UINT64_MAX, UINT64_MAX, inaccessible}, INVALID_HANDLE) ||
	    !invoke(U"uquVH4-Du78", {1, 0, 0, 0}, 0) ||
	    !invoke(U"kGVLc3htQE8", {1, inaccessible, UINT64_MAX, 0}, INVALID_HANDLE) ||
	    !invoke(U"zgXifHT9ErY", {1, 0, 0, 0}, INVALID_HANDLE) ||
	    !invoke(U"N1bEoJ4SRw4", {1, 0, inaccessible, UINT64_MAX}, INVALID_HANDLE) ||
	    !invoke(U"Up36PTk687E", open, 1) ||
	    !invoke(U"N1bEoJ4SRw4", {1, UINT64_MAX, 0, inaccessible}, UNSUPPORTED_OPERATION) ||
	    !invoke(U"uquVH4-Du78", {1, 0, 0, 0}, 0)) return false;

	// SetBufferAttribute is the sole seven-GP export: permit exactly its first
	// spill through the checked VM reader, then repeat with that spill guarded.
	g_host_call_guest_memory_read = [](Lumen::Cpu::X64State* state, uint64_t base, size_t offset,
	                                   void* destination, size_t size)
	{
		++g_stack_read_attempts;
		const uint64_t address = base <= UINT64_MAX - offset ? base + offset : base;
		Memory::GuestDataLease lease;
		if (offset == 8 && size == 8 && base <= UINT64_MAX - offset &&
		    Memory::TryAcquireGuestDataLease(address, size, Memory::Mode::Read, &lease))
		{
			std::memcpy(destination, reinterpret_cast<const void*>(address), size);
			return true;
		}
		state->memory_fault_address = address;
		state->memory_fault_size = static_cast<uint32_t>(size);
		state->memory_fault_write = false;
		return false;
	};
	const auto* attribute = symbols.Find({U"i6-sR91Wt-4", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func});
	if (!Check(attribute != nullptr, "registered attribute export was not found")) return false;
	// Both buffers include sentinels beyond the existing 40-byte local record.
	alignas(8) std::array<uint8_t, 64> expected_attribute, actual_attribute;
	expected_attribute.fill(0xa7);
	actual_attribute = expected_attribute;
	Video::VideoOutSetBufferAttribute(reinterpret_cast<Video::VideoOutBufferAttribute*>(expected_attribute.data()),
	                                 0x80000000, 0, 0, 1280, 720, 1920);
	Lumen::Cpu::X64State state {Lumen::Cpu::X64State::HostCallOnly {}};
	state.gpr[0] = result_sentinel;
	state.gpr[4] = stack_pointer - 16;
	state.gpr[7] = reinterpret_cast<uint64_t>(actual_attribute.data());
	state.gpr[6] = 0x80000000;
	state.gpr[2] = 0;
	state.gpr[1] = 0;
	state.gpr[8] = 1280;
	state.gpr[9] = 720;
	const uint64_t pitch = UINT64_C(0xaabbccdd00000780);
	std::memcpy(reinterpret_cast<void*>(state.gpr[4]), &return_address, sizeof(return_address));
	std::memcpy(reinterpret_cast<void*>(state.gpr[4] + 8), &pitch, sizeof(pitch));
	if (!Check(InvokeHostCall(&symbols, attribute->vaddr, &state) && g_stack_read_attempts == 1 &&
	           state.memory_fault_size == 0 && state.gpr[0] == result_sentinel && actual_attribute == expected_attribute,
	           "seven-GP void call read excess stack or changed its output/RAX contract")) return false;
	actual_attribute.fill(0x5b);
	const auto unchanged = actual_attribute;
	state.gpr[4] = stack_pointer;
	g_stack_read_attempts = 0;
	return Check(InvokeHostCall(&symbols, attribute->vaddr, &state) && g_stack_read_attempts == 1 &&
	             state.memory_fault_address == inaccessible && state.memory_fault_size == 8 &&
	             !state.memory_fault_write && state.gpr[0] == result_sentinel && actual_attribute == unchanged,
	             "protected seventh argument invoked the wrapper or changed RAX/output");
}

bool MemoryGuards(size_t page_size)
{
	Mapping mapping(page_size * 2);
	if (!Check(mapping.Valid(), "guard fixture allocation failed")) return false;
	const auto original = Pattern(page_size * 2);
	std::memcpy(mapping.Data(), original.data(), original.size());
	if (!Check(mapping.Protect(0, page_size, Memory::Mode::Read), "read-only protection failed") ||
	    !Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(16), 1.0f) == INVALID_ADDRESS,
	           "read-only destination was accepted") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::NoAccess), "no-access protection failed") ||
	    !Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(16), 1.0f) == INVALID_ADDRESS &&
	           Video::VideoOutColorSettingsSetGamma(mapping.Data(16), 3.0f) == INVALID_VALUE,
	           "protected destination or invalid-value precedence was wrong") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::ReadWrite), "guard fixture restore failed") ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0,
	           "failed protected write changed bytes")) return false;

	if (!Check(mapping.Protect(page_size, page_size, Memory::Mode::NoAccess), "following guard page failed")) return false;
	for (size_t accessible: {size_t(1), size_t(2), size_t(3)})
		if (!Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(page_size - accessible), 1.0f) == INVALID_ADDRESS,
		           "write crossing the guard page was accepted") ||
		    !Check(std::memcmp(mapping.Data(), original.data(), page_size) == 0,
		           "rejected crossing write altered its accessible prefix")) return false;

	// Exactly four writable bytes suffice: the declared reserved cells may be
	// inaccessible because this local helper neither reads nor writes them.
	const float gamma = 1.75f;
	auto expected = original;
	std::memcpy(expected.data() + page_size - sizeof(gamma), &gamma, sizeof(gamma));
	if (!Check(Video::VideoOutColorSettingsSetGamma(mapping.Data(page_size - sizeof(gamma)), gamma) == 0,
	           "four-byte output required access to reserved cells") ||
	    !Check(mapping.Protect(page_size, page_size, Memory::Mode::ReadWrite), "following guard restore failed") ||
	    !Check(std::memcmp(mapping.Data(), expected.data(), expected.size()) == 0,
	           "exact-boundary write touched bytes outside the gamma field")) return false;

	const uint64_t freed = mapping.Address();
	if (!Check(mapping.Free(), "guard fixture free failed")) return false;
	for (uint64_t address: {uint64_t(0), uint64_t(1), freed, std::numeric_limits<uint64_t>::max() - 1})
		if (!Check(Video::VideoOutColorSettingsSetGamma(reinterpret_cast<void*>(address), 1.0f) == INVALID_ADDRESS,
		           "null, unmapped or overflowing destination was accepted")) return false;
	return true;
}

bool AdjustValues(int handle, size_t page_size)
{
	Mapping mapping(page_size);
	if (!Check(mapping.Valid(), "AdjustColor value allocation failed")) return false;
	struct ValueCase { float gamma; int result; };
	const ValueCase cases[] {
	    {1.0f, 0}, {0.1f, UNSUPPORTED}, {2.0f, UNSUPPORTED},
	    {std::nextafter(1.0f, 0.0f), UNSUPPORTED}, {std::nextafter(1.0f, 2.0f), UNSUPPORTED},
	    {std::nextafter(0.1f, 0.0f), INVALID_VALUE}, {std::nextafter(2.0f, 3.0f), INVALID_VALUE},
	    {0.0f, INVALID_VALUE}, {-0.0f, INVALID_VALUE}, {-1.0f, INVALID_VALUE},
	    {std::numeric_limits<float>::infinity(), INVALID_VALUE},
	    {-std::numeric_limits<float>::infinity(), INVALID_VALUE},
	    {std::numeric_limits<float>::quiet_NaN(), INVALID_VALUE}, {1.0f, 0},
	};
	for (size_t offset: {size_t(16), size_t(19)})
		for (const auto& item: cases)
		{
			auto expected = Pattern(page_size);
			std::memcpy(expected.data() + offset, &item.gamma, sizeof(item.gamma));
			std::memcpy(mapping.Data(), expected.data(), expected.size());
			if (!Check(mapping.Protect(0, page_size, Memory::Mode::Read), "AdjustColor read-only protection failed") ||
			    !Check(Video::VideoOutAdjustColor(handle, mapping.Data(offset)) == item.result,
			           "AdjustColor value, identity or unsupported result was wrong") ||
			    !Check(std::memcmp(mapping.Data(), expected.data(), expected.size()) == 0,
			           "AdjustColor changed its input or surrounding bytes") ||
			    !Check(mapping.Protect(0, page_size, Memory::Mode::ReadWrite), "AdjustColor value protection restore failed")) return false;
		}
	return true;
}

bool AdjustMemoryGuards(int handle, size_t page_size)
{
	Mapping mapping(page_size * 2);
	if (!Check(mapping.Valid(), "AdjustColor guard allocation failed")) return false;
	auto expected = Pattern(page_size * 2);
	const float neutral = 1.0f;
	std::memcpy(expected.data() + page_size - sizeof(neutral), &neutral, sizeof(neutral));
	std::memcpy(mapping.Data(), expected.data(), expected.size());
	if (!Check(mapping.Protect(0, page_size, Memory::Mode::Read) &&
	           mapping.Protect(page_size, page_size, Memory::Mode::NoAccess), "AdjustColor guards could not be installed") ||
	    !Check(Video::VideoOutAdjustColor(handle, mapping.Data(page_size - sizeof(neutral))) == 0,
	           "AdjustColor required reserved bytes or write permission") ||
	    !Check(Video::VideoOutAdjustColor(handle, mapping.Data(page_size)) == INVALID_ADDRESS,
	           "AdjustColor accepted an unreadable input")) return false;
	for (size_t accessible: {size_t(1), size_t(2), size_t(3)})
		if (!Check(Video::VideoOutAdjustColor(handle, mapping.Data(page_size - accessible)) == INVALID_ADDRESS,
		           "AdjustColor accepted a partially readable gamma")) return false;
	if (!Check(std::memcmp(mapping.Data(), expected.data(), page_size) == 0,
	           "AdjustColor guard cases changed accessible bytes") ||
	    !Check(mapping.Protect(0, page_size * 2, Memory::Mode::ReadWrite), "AdjustColor guards restore failed") ||
	    !Check(std::memcmp(mapping.Data(), expected.data(), expected.size()) == 0,
	           "AdjustColor changed reserved or inaccessible bytes")) return false;
	const uint64_t freed = mapping.Address();
	if (!Check(mapping.Free(), "AdjustColor guard free failed")) return false;
	for (uint64_t address: {uint64_t(0), uint64_t(1), freed, std::numeric_limits<uint64_t>::max() - 1})
		if (!Check(Video::VideoOutAdjustColor(handle, reinterpret_cast<const void*>(address)) == INVALID_ADDRESS,
		           "AdjustColor accepted null, unmapped or overflowing input")) return false;
	return true;
}

bool OpenParameterRejection(size_t page_size)
{
	Mapping mapping(page_size);
	if (!Check(mapping.Valid(), "VideoOutOpen parameter allocation failed")) return false;
	const uint32_t observed_candidate[4] {16, 0, 0, 1};
	std::memcpy(mapping.Data(), observed_candidate, sizeof(observed_candidate));

	auto reject_without_consuming_handle = [](const void* param, const char* message)
	{
		if (!Check(Video::VideoOutOpen(255, 0, 0, param) == UNSUPPORTED_OPERATION, message)) return false;
		const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
		return Check(handle == 1, "VideoOutOpen rejection consumed the available handle") &&
		       Check(Video::VideoOutClose(handle) == 0, "VideoOutOpen rejection probe close failed");
	};

	if (!reject_without_consuming_handle(mapping.Data(), "VideoOutOpen accepted a mapped non-null parameter") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::Read), "VideoOutOpen read-only protection failed") ||
	    !reject_without_consuming_handle(mapping.Data(), "VideoOutOpen accepted a read-only non-null parameter") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::NoAccess), "VideoOutOpen no-access protection failed") ||
	    !reject_without_consuming_handle(mapping.Data(), "VideoOutOpen read or accepted a no-access parameter") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::ReadWrite), "VideoOutOpen protection restore failed")) return false;

	const uint64_t freed = mapping.Address();
	if (!Check(mapping.Free(), "VideoOutOpen parameter free failed") ||
	    !reject_without_consuming_handle(reinterpret_cast<const void*>(uint64_t(1)),
	                                     "VideoOutOpen read or accepted a low unmapped parameter") ||
	    !reject_without_consuming_handle(reinterpret_cast<const void*>(freed),
	                                     "VideoOutOpen read or accepted a freed parameter") ||
	    !reject_without_consuming_handle(
	        reinterpret_cast<const void*>(std::numeric_limits<uint64_t>::max()),
	        "VideoOutOpen read or accepted an overflowing parameter")) return false;

	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle == 1, "VideoOutOpen normal open failed after parameter rejection") ||
	    !Check(Video::VideoOutOpen(255, 0, 0, reinterpret_cast<const void*>(uint64_t(1))) ==
	               UNSUPPORTED_OPERATION,
	           "VideoOutOpen changed non-null rejection while a port was open") ||
	    !Check(Video::VideoOutOpen(255, 0, 0, nullptr) == Video::VIDEO_OUT_ERROR_RESOURCE_BUSY,
	           "VideoOutOpen did not preserve resource-busy behavior")) return false;
	return Check(Video::VideoOutClose(handle) == 0, "VideoOutOpen active rejection probe close failed");
}

bool OpenParameterExperiment(size_t page_size, bool enabled)
{
	Mapping output(page_size);
	Mapping parameter(page_size * 2);
	if (!Check(output.Valid() && parameter.Valid(), "Open parameter experiment allocation failed")) return false;
	auto* status = reinterpret_cast<Video::VideoOutResolutionStatus*>(output.Data());
	Video::VideoOutInit(1280, 720);

	// The first call deliberately carries no opaque parameter. Changing the
	// environment afterwards must not change the process's selected behavior.
	const int first = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(first == 1 && Video::VideoOutGetResolutionStatus(first, status) == 0,
	           "Open parameter selector changed the initial null-parameter open") ||
	    !Check(setenv("LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE", enabled ? "0" : "1", 1) == 0,
	           "Open parameter selector mutation failed") ||
	    !Check(Video::VideoOutOpen(255, 0, 0, nullptr) == Video::VIDEO_OUT_ERROR_RESOURCE_BUSY,
	           "Open parameter selector changed null-parameter resource-busy behavior") ||
	    !Check(Video::VideoOutClose(first) == 0, "Open parameter initial close failed")) return false;

	const auto exercise = [&](const void* opaque)
	{
		const int result = Video::VideoOutOpen(255, 0, 0, opaque);
		if (!Check(result == (enabled ? 1 : UNSUPPORTED_OPERATION),
		           "Open parameter selection was wrong or changed after the first null call")) return false;
		const int handle = enabled ? result : Video::VideoOutOpen(255, 0, 0, nullptr);
		if (!Check(handle == 1 && Video::VideoOutGetResolutionStatus(handle, status) == 0,
		           "Open parameter policy failed to allocate a real handle or consumed one on rejection") ||
		    !Check(Video::VideoOutOpen(255, 0, 0, nullptr) == Video::VIDEO_OUT_ERROR_RESOURCE_BUSY &&
		               Video::VideoOutOpen(255, 0, 0, opaque) ==
		                   (enabled ? Video::VIDEO_OUT_ERROR_RESOURCE_BUSY : UNSUPPORTED_OPERATION),
		           "Open parameter policy changed busy handling or default rejection precedence") ||
		    !Check(Video::VideoOutClose(handle) == 0 &&
		               Video::VideoOutGetResolutionStatus(handle, status) == INVALID_HANDLE &&
		               Video::VideoOutClose(handle) == INVALID_HANDLE,
		           "Open parameter handle did not follow the normal close lifetime")) return false;
		const int reopened = Video::VideoOutOpen(255, 0, 0, enabled ? opaque : nullptr);
		return Check(reopened == 1 && Video::VideoOutGetResolutionStatus(reopened, status) == 0,
		             "Open parameter handle could not be reopened") &&
		       Check(Video::VideoOutClose(reopened) == 0, "Open parameter reopened handle close failed");
	};

	// These are arbitrary byte payloads, not proposed records or field layouts.
	auto bytes = Pattern(page_size * 2);
	std::memcpy(parameter.Data(), bytes.data(), bytes.size());
	if (!exercise(parameter.Data(23)) ||
	    !Check(std::memcmp(parameter.Data(), bytes.data(), bytes.size()) == 0,
	           "Open parameter policy changed the first opaque payload")) return false;
	for (auto& byte: bytes) byte ^= 0x5a;
	std::memcpy(parameter.Data(), bytes.data(), bytes.size());
	if (!exercise(parameter.Data(41)) ||
	    !Check(std::memcmp(parameter.Data(), bytes.data(), bytes.size()) == 0,
	           "Open parameter policy changed the second opaque payload") ||
	    !Check(parameter.Protect(0, page_size * 2, Memory::Mode::Read),
	           "Open parameter read-only protection failed") ||
	    !exercise(parameter.Data(41)) ||
	    !Check(parameter.Protect(page_size, page_size, Memory::Mode::NoAccess),
	           "Open parameter guard page protection failed") ||
	    !exercise(parameter.Data(page_size - 1)) || !exercise(parameter.Data(page_size)) ||
	    !Check(parameter.Protect(0, page_size, Memory::Mode::NoAccess),
	           "Open parameter no-access protection failed") ||
	    !exercise(parameter.Data()) ||
	    !Check(parameter.Protect(0, page_size * 2, Memory::Mode::ReadWrite),
	           "Open parameter protection restore failed") ||
	    !Check(std::memcmp(parameter.Data(), bytes.data(), bytes.size()) == 0,
	           "Open parameter policy changed protected opaque bytes")) return false;
	const uint64_t freed = parameter.Address();
	if (!Check(parameter.Free(), "Open parameter unmap failed")) return false;
	for (uint64_t address: {uint64_t(1), freed, std::numeric_limits<uint64_t>::max()})
		if (!exercise(reinterpret_cast<const void*>(address))) return false;
	return true;
}

bool DeviceCapabilityFailClosed(size_t page_size)
{
	Mapping output(page_size);
	if (!Check(output.Valid(), "device capability output allocation failed")) return false;
	const auto original = Pattern(page_size);
	std::memcpy(output.Data(), original.data(), original.size());
	const void* unreadable = reinterpret_cast<const void*>(uint64_t(1));

	if (!Check(Video::VideoOutGetDeviceCapabilityInfo(-1, output.Data(32), 8) == INVALID_HANDLE &&
	           Video::VideoOutGetDeviceCapabilityInfo(0, const_cast<void*>(unreadable), 8) == INVALID_HANDLE,
	           "device capability accepted an invalid or unopened handle") ||
	    !Check(Video::VideoOutIsFlipPending(-1) == INVALID_HANDLE &&
	           Video::VideoOutIsFlipPending(0) == INVALID_HANDLE,
	           "pending query accepted an invalid or unopened handle") ||
	    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
	           "invalid device capability query changed its output")) return false;

	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle == 1, "device capability port open failed") ||
	    !Check(Video::VideoOutGetDeviceCapabilityInfo(handle, output.Data(32), 8) == UNSUPPORTED_OPERATION &&
	           Video::VideoOutGetDeviceCapabilityInfo(handle, nullptr, 0) == UNSUPPORTED_OPERATION &&
	           Video::VideoOutGetDeviceCapabilityInfo(handle, const_cast<void*>(unreadable),
	                                                   std::numeric_limits<size_t>::max()) == UNSUPPORTED_OPERATION,
	           "device capability guessed a record or inspected an opaque output") ||
	    !Check(Video::VideoOutIsFlipPending(handle) == UNSUPPORTED_OPERATION,
	           "pending query guessed a valid-handle state encoding") ||
	    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
	           "unsupported device capability query changed its output") ||
	    !Check(output.Protect(0, page_size, Memory::Mode::NoAccess),
	           "device capability output guard failed") ||
	    !Check(Video::VideoOutGetDeviceCapabilityInfo(handle, output.Data(32), 8) == UNSUPPORTED_OPERATION,
	           "device capability read or wrote a guarded output") ||
	    !Check(output.Protect(0, page_size, Memory::Mode::ReadWrite),
	           "device capability output guard restore failed") ||
	    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
	           "guarded device capability query changed output bytes") ||
	    !Check(Video::VideoOutClose(handle) == 0, "device capability port close failed") ||
	    !Check(Video::VideoOutGetDeviceCapabilityInfo(handle, output.Data(32), 8) == INVALID_HANDLE,
	           "device capability accepted a closed handle") ||
	    !Check(Video::VideoOutIsFlipPending(handle) == INVALID_HANDLE,
	           "pending query accepted a closed handle")) return false;
	return true;
}

bool ConfigureOutputModeFailClosed(size_t page_size)
{
	Mapping records(page_size * 2);
	if (!Check(records.Valid(), "configuration record allocation failed")) return false;
	const auto original = Pattern(page_size * 2);
	std::memcpy(records.Data(), original.data(), original.size());
	const std::array<const void*, 5> pointers {
	    nullptr, records.Data(3), records.Data(page_size + 5), reinterpret_cast<const void*>(uint64_t(1)),
	    reinterpret_cast<const void*>(std::numeric_limits<uint64_t>::max()),
	};
	const std::array<uint32_t, 5> sizes {0, 1, 32, 73, std::numeric_limits<uint32_t>::max()};
	const auto exercise = [&](int handle, int expected)
	{
		for (uint32_t reserved: {0u, 1u, std::numeric_limits<uint32_t>::max()})
			for (size_t index = 0; index != pointers.size(); ++index)
				if (!Check(Video::VideoOutConfigureOutputMode(handle, reserved, pointers[index], pointers[4 - index],
				                                                sizes[index], sizes[4 - index]) == expected,
				           "configuration inspected opaque input or changed its handle-only error policy")) return false;
		return true;
	};
	for (int handle: {std::numeric_limits<int>::min(), -1, 0, 1, std::numeric_limits<int>::max()})
		if (!exercise(handle, INVALID_HANDLE)) return false;
	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle == 1, "configuration port open failed") || !exercise(handle, UNSUPPORTED_OPERATION) ||
	    !Check(std::memcmp(records.Data(), original.data(), original.size()) == 0,
	           "configuration changed writable opaque records") ||
	    !Check(records.Protect(0, page_size * 2, Memory::Mode::Read), "configuration read-only protection failed") ||
	    !exercise(handle, UNSUPPORTED_OPERATION) ||
	    !Check(std::memcmp(records.Data(), original.data(), original.size()) == 0,
	           "configuration changed read-only opaque records") ||
	    !Check(records.Protect(0, page_size * 2, Memory::Mode::NoAccess), "configuration no-access protection failed") ||
	    !exercise(handle, UNSUPPORTED_OPERATION) || !exercise(-1, INVALID_HANDLE) ||
	    !Check(records.Protect(0, page_size * 2, Memory::Mode::ReadWrite), "configuration protection restore failed") ||
	    !Check(std::memcmp(records.Data(), original.data(), original.size()) == 0,
	           "configuration changed guarded opaque records") ||
	    !Check(records.Free(), "configuration record unmap failed") ||
	    !exercise(handle, UNSUPPORTED_OPERATION) ||
	    !Check(Video::VideoOutClose(handle) == 0, "configuration port close failed") ||
	    !exercise(handle, INVALID_HANDLE)) return false;
	const int reopened = Video::VideoOutOpen(255, 0, 0, nullptr);
	return Check(reopened == 1, "configuration port reopen failed") && exercise(reopened, UNSUPPORTED_OPERATION) &&
	       Check(Video::VideoOutClose(reopened) == 0, "configuration reopened port close failed");
}

bool ResolutionStatusErrors(size_t page_size)
{
	Mapping output(page_size);
	if (!Check(output.Valid(), "resolution status allocation failed")) return false;
	const auto original = Pattern(page_size);
	auto expected = original;
	constexpr size_t resolution_status_size = 48;
	if (!Check(page_size >= resolution_status_size, "resolution status page is smaller than its ABI record")) return false;
	std::memset(expected.data(), 0, resolution_status_size);
	const uint32_t width = 1280;
	const uint32_t height = 720;
	const uint64_t refresh_rate = 3;
	const float screen_size = 50.0f;
	std::memcpy(expected.data() + 0, &width, sizeof(width));
	std::memcpy(expected.data() + 4, &height, sizeof(height));
	std::memcpy(expected.data() + 8, &width, sizeof(width));
	std::memcpy(expected.data() + 12, &height, sizeof(height));
	std::memcpy(expected.data() + 16, &refresh_rate, sizeof(refresh_rate));
	std::memcpy(expected.data() + 24, &screen_size, sizeof(screen_size));
	std::memcpy(output.Data(), original.data(), original.size());
	auto* status = reinterpret_cast<Video::VideoOutResolutionStatus*>(output.Data());
	if (!Check(Video::VideoOutGetResolutionStatus(-1, status) == INVALID_HANDLE,
	           "resolution status accepted an invalid handle") ||
	    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
	           "invalid-handle resolution query changed its output")) return false;

	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle == 1, "resolution status port open failed") ||
	    !Check(Video::VideoOutGetResolutionStatus(handle, status) == 0,
	           "resolution status rejected a writable output") ||
	    !Check(std::memcmp(output.Data(), expected.data(), expected.size()) == 0,
	           "resolution status did not deterministically replace its full 48-byte record") ||
	    !Check(output.Protect(0, page_size, Memory::Mode::Read), "resolution read-only protection failed") ||
	    !Check(Video::VideoOutGetResolutionStatus(handle, status) == INVALID_ADDRESS,
	           "resolution status wrote through a read-only output") ||
	    !Check(output.Protect(0, page_size, Memory::Mode::NoAccess), "resolution no-access protection failed") ||
	    !Check(Video::VideoOutGetResolutionStatus(handle, status) == INVALID_ADDRESS,
	           "resolution status wrote through a no-access output") ||
	    !Check(Video::VideoOutGetResolutionStatus(-1, status) == INVALID_HANDLE,
	           "resolution status inspected output before rejecting the handle") ||
	    !Check(output.Protect(0, page_size, Memory::Mode::ReadWrite), "resolution protection restore failed")) return false;

	const uint64_t freed = output.Address();
	if (!Check(output.Free(), "resolution status output free failed")) return false;
	for (uint64_t address: {uint64_t(1), freed, std::numeric_limits<uint64_t>::max()})
	{
		if (!Check(Video::VideoOutGetResolutionStatus(
		               handle, reinterpret_cast<Video::VideoOutResolutionStatus*>(address)) == INVALID_ADDRESS,
		           "resolution status accepted an unmapped or overflowing output")) return false;
	}
	return Check(Video::VideoOutClose(handle) == 0, "resolution status port close failed");
}

bool InvalidHandleErrors(size_t page_size)
{
	Mapping output(page_size);
	if (!Check(output.Valid(), "invalid-handle output allocation failed")) return false;
	const auto original = Pattern(page_size);
	std::memcpy(output.Data(), original.data(), original.size());
	auto* flip = reinterpret_cast<Video::VideoOutFlipStatus*>(output.Data());
	auto* vblank = reinterpret_cast<Video::VideoOutVblankStatus*>(output.Data());
	if (!Check(Video::VideoOutSetFlipRate(-1, 0) == INVALID_HANDLE &&
	               Video::VideoOutSetFlipRate(-1, -1) == INVALID_HANDLE,
	           "SetFlipRate did not reject an invalid handle") ||
	    !Check(Video::VideoOutAddFlipEvent(nullptr, -1, nullptr) == INVALID_HANDLE,
	           "AddFlipEvent did not reject an invalid handle") ||
	    !Check(Video::VideoOutAddVblankEvent(nullptr, -1, nullptr) == INVALID_HANDLE,
	           "AddVblankEvent did not reject an invalid handle") ||
	    !Check(Video::VideoOutSubmitFlip(-1, 0, 1, 0) == INVALID_HANDLE,
	           "SubmitFlip did not reject an invalid handle") ||
	    !Check(Video::VideoOutGetFlipStatus(-1, flip) == INVALID_HANDLE &&
	               Video::VideoOutGetFlipStatus(-1, nullptr) == INVALID_ADDRESS,
	           "GetFlipStatus invalid-handle/address precedence was wrong") ||
	    !Check(Video::VideoOutGetVblankStatus(-1, vblank) == INVALID_HANDLE &&
	               Video::VideoOutGetVblankStatus(-1, nullptr) == INVALID_ADDRESS,
	           "GetVblankStatus invalid-handle/address precedence was wrong") ||
	    !Check(Video::VideoOutSetWindowModeMargins(-1, 0, 0) == INVALID_HANDLE,
	           "SetWindowModeMargins did not reject an invalid handle") ||
	    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
	           "invalid-handle VideoOut calls changed their output")) return false;
	return true;
}

bool StatusMemoryGuards(int handle, size_t page_size)
{
	auto exercise = [handle, page_size](auto invoke) -> bool
	{
		Mapping output(page_size * 2);
		if (!Check(output.Valid(), "status output allocation failed")) return false;
		const auto original = Pattern(page_size * 2);
		std::memcpy(output.Data(), original.data(), original.size());
		if (!Check(invoke(handle, output.Data(32)) == 0, "status rejected writable output")) return false;

		std::memcpy(output.Data(), original.data(), original.size());
		if (!Check(output.Protect(0, page_size, Memory::Mode::Read), "status read-only protection failed") ||
		    !Check(invoke(handle, output.Data(32)) == INVALID_ADDRESS,
		           "status wrote through a read-only output") ||
		    !Check(output.Protect(0, page_size, Memory::Mode::ReadWrite), "status read-only restore failed") ||
		    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
		           "failed read-only status write changed output")) return false;

		if (!Check(output.Protect(0, page_size, Memory::Mode::NoAccess), "status no-access protection failed") ||
		    !Check(invoke(handle, output.Data(32)) == INVALID_ADDRESS,
		           "status wrote through a no-access output") ||
		    !Check(output.Protect(0, page_size, Memory::Mode::ReadWrite), "status no-access restore failed") ||
		    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
		           "failed no-access status write changed output")) return false;

		if (!Check(output.Protect(page_size, page_size, Memory::Mode::NoAccess),
		           "status following guard page failed") ||
		    !Check(invoke(handle, output.Data(page_size - 1)) == INVALID_ADDRESS,
		           "status accepted a partially writable output") ||
		    !Check(std::memcmp(output.Data(), original.data(), page_size) == 0,
		           "failed partial status write changed its writable prefix") ||
		    !Check(output.Protect(page_size, page_size, Memory::Mode::ReadWrite),
		           "status following guard restore failed") ||
		    !Check(std::memcmp(output.Data(), original.data(), original.size()) == 0,
		           "failed partial status write changed output")) return false;

		for (uint64_t address: {uint64_t(1), std::numeric_limits<uint64_t>::max()})
		{
			if (!Check(invoke(handle, reinterpret_cast<void*>(address)) == INVALID_ADDRESS,
			           "status accepted an unmapped or overflowing output")) return false;
		}
		const uint64_t freed = output.Address();
		return Check(output.Free(), "status output free failed") &&
		       Check(invoke(handle, reinterpret_cast<void*>(freed)) == INVALID_ADDRESS,
		             "status accepted a freed output");
	};

	const auto flip = [](int current_handle, void* output)
	{
		return Video::VideoOutGetFlipStatus(
		    current_handle, reinterpret_cast<Video::VideoOutFlipStatus*>(output));
	};
	const auto vblank = [](int current_handle, void* output)
	{
		return Video::VideoOutGetVblankStatus(
		    current_handle, reinterpret_cast<Video::VideoOutVblankStatus*>(output));
	};
	return exercise(flip) && exercise(vblank);
}

bool ConcurrentCloseStatus(size_t page_size)
{
	auto exercise = [page_size](auto invoke) -> bool
	{
		Mapping output(page_size);
		if (!Check(output.Valid(), "concurrent status output allocation failed")) return false;
		const auto original = Pattern(page_size);

		const int baseline_handle = Video::VideoOutOpen(255, 0, 0, nullptr);
		if (!Check(baseline_handle == 1, "concurrent status baseline open failed")) return false;
		std::memcpy(output.Data(), original.data(), original.size());
		if (!Check(invoke(baseline_handle, output.Data(32)) == 0,
		           "concurrent status baseline query failed")) return false;
		std::vector<uint8_t> expected(output.Data(), output.Data() + page_size);
		if (!Check(Video::VideoOutClose(baseline_handle) == 0,
		           "concurrent status baseline close failed")) return false;

		for (unsigned iteration = 0; iteration != 8; ++iteration)
		{
			const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
			if (!Check(handle == 1, "concurrent status iteration open failed")) return false;
			std::memcpy(output.Data(), original.data(), original.size());
			std::atomic_bool ready {false};
			std::atomic_bool start {false};
			int status_result = 0;
			int rate_result = 0;
			int flip_event_result = 0;
			int vblank_event_result = 0;
			int submit_result = 0;
			int margins_result = 0;
			std::thread worker([&]()
			{
				ready.store(true, std::memory_order_release);
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				status_result = invoke(handle, output.Data(32));
				rate_result = Video::VideoOutSetFlipRate(handle, 0);
				flip_event_result = Video::VideoOutAddFlipEvent(nullptr, handle, nullptr);
				vblank_event_result = Video::VideoOutAddVblankEvent(nullptr, handle, nullptr);
				submit_result = Video::VideoOutSubmitFlip(handle, 16, 1, 0);
				margins_result = Video::VideoOutSetWindowModeMargins(handle, 0, 0);
			});
			while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
			start.store(true, std::memory_order_release);
			if ((iteration & 1u) != 0) std::this_thread::yield();
			const int close_result = Video::VideoOutClose(handle);
			worker.join();

			const bool status_ok = status_result == 0
			                               ? std::memcmp(output.Data(), expected.data(), expected.size()) == 0
			                               : status_result == INVALID_HANDLE &&
			                                     std::memcmp(output.Data(), original.data(), original.size()) == 0;
			if (!Check(close_result == 0, "concurrent status close failed") ||
			    !Check(status_ok, "concurrent status returned or published a partial result") ||
			    !Check(rate_result == 0 || rate_result == INVALID_HANDLE,
			           "SetFlipRate close race returned an unexpected result") ||
			    !Check(flip_event_result == Video::VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE ||
			               flip_event_result == INVALID_HANDLE,
			           "AddFlipEvent close race returned an unexpected result") ||
			    !Check(vblank_event_result == Video::VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE ||
			               vblank_event_result == INVALID_HANDLE,
			           "AddVblankEvent close race returned an unexpected result") ||
			    !Check(submit_result == Video::VIDEO_OUT_ERROR_INVALID_INDEX || submit_result == INVALID_HANDLE,
			           "SubmitFlip close race returned an unexpected result") ||
			    !Check(margins_result == 0 || margins_result == INVALID_HANDLE,
			           "SetWindowModeMargins close race returned an unexpected result")) return false;
		}
		return true;
	};

	const auto flip = [](int handle, void* output)
	{
		return Video::VideoOutGetFlipStatus(handle, reinterpret_cast<Video::VideoOutFlipStatus*>(output));
	};
	const auto vblank = [](int handle, void* output)
	{
		return Video::VideoOutGetVblankStatus(handle, reinterpret_cast<Video::VideoOutVblankStatus*>(output));
	};
	return exercise(flip) && exercise(vblank);
}

bool AdjustLifecycle(size_t page_size)
{
	// This suite initializes only VideoOut's administrative context. It never
	// creates a window, submits a flip or claims an applied display transform.
	Mapping record(page_size);
	if (!Check(record.Valid(), "AdjustColor lifecycle allocation failed")) return false;
	const float neutral = 1.0f;
	std::memcpy(record.Data(), &neutral, sizeof(neutral));
	const void* unreadable = reinterpret_cast<const void*>(uint64_t(1));
	if (!Check(Video::VideoOutAdjustColor(-1, nullptr) == INVALID_ADDRESS &&
	           Video::VideoOutAdjustColor(1, record.Data()) == INVALID_HANDLE &&
	           Video::VideoOutAdjustColor(1, unreadable) == INVALID_HANDLE,
	           "AdjustColor missing-context precedence was wrong") ||
	    !Check(Video::VideoOutGetResolutionStatus(
	               1, reinterpret_cast<Video::VideoOutResolutionStatus*>(record.Data())) == INVALID_HANDLE &&
	               Video::VideoOutGetResolutionStatus(1, nullptr) == INVALID_ADDRESS,
	           "resolution status missing-context precedence was wrong") ||
	    !Check(Video::VideoOutConfigureOutputMode(1, UINT32_MAX, unreadable, nullptr, UINT32_MAX, 0) == INVALID_HANDLE &&
	               Video::VideoOutConfigureOutputMode(-1, 0, nullptr, unreadable, 0, UINT32_MAX) == INVALID_HANDLE,
	           "configuration accepted a missing context or inspected an opaque pointer")) return false;
	Video::VideoOutInit(1280, 720);
	if (!RegisteredGpStackBoundary(page_size) || !OpenParameterRejection(page_size) || !DeviceCapabilityFailClosed(page_size) ||
	    !ConfigureOutputModeFailClosed(page_size) || !ResolutionStatusErrors(page_size) ||
	    !InvalidHandleErrors(page_size)) return false;
	if (!Check(Video::VideoOutAdjustColor(0, nullptr) == INVALID_ADDRESS &&
	           Video::VideoOutAdjustColor(0, unreadable) == INVALID_HANDLE,
	           "AdjustColor unopened-handle precedence was wrong")) return false;
	const int handle = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(handle >= 0, "AdjustColor port open failed")) return false;
	bool passed = true;
	for (int invalid: {std::numeric_limits<int>::min(), -1, std::numeric_limits<int>::max()})
		passed = Check(Video::VideoOutAdjustColor(invalid, nullptr) == INVALID_ADDRESS &&
		               Video::VideoOutAdjustColor(invalid, record.Data()) == INVALID_HANDLE &&
		               Video::VideoOutAdjustColor(invalid, unreadable) == INVALID_HANDLE,
		               "AdjustColor invalid-handle precedence was wrong") && passed;
	passed = StatusMemoryGuards(handle, page_size) && passed;
	passed = AdjustValues(handle, page_size) && passed;
	passed = AdjustMemoryGuards(handle, page_size) && passed;
	passed = Check(Video::VideoOutClose(handle) == 0, "AdjustColor port close failed") && passed;
	if (!passed) return false;
	if (!Check(Video::VideoOutAdjustColor(handle, nullptr) == INVALID_ADDRESS &&
	           Video::VideoOutAdjustColor(handle, record.Data()) == INVALID_HANDLE &&
	           Video::VideoOutAdjustColor(handle, unreadable) == INVALID_HANDLE,
	           "AdjustColor accepted a closed handle or read it before validating")) return false;
	// Close the first port before reopening: exhaustion behavior of Open is a
	// separate preexisting limitation and is not exercised by this fixture.
	const int reopened = Video::VideoOutOpen(255, 0, 0, nullptr);
	if (!Check(reopened >= 0, "AdjustColor port reopen failed")) return false;
	passed = Check(Video::VideoOutAdjustColor(reopened, record.Data()) == 0,
	               "AdjustColor identity failed after reopen");
	if (!Check(Video::VideoOutClose(reopened) == 0, "AdjustColor reopened port close failed") || !passed) return false;
	return ConcurrentCloseStatus(page_size);
}
} // namespace

int main(int argc, char** argv)
{
	const bool open_enabled = argc == 2 && std::strcmp(argv[1], "--open-param-ignore=enabled") == 0;
	const bool open_disabled = argc == 2 && std::strcmp(argv[1], "--open-param-ignore=disabled") == 0;
	if (argc != 1 && !open_enabled && !open_disabled)
	{
		std::fputs("VideoOutGammaTest: unexpected arguments\n", stderr);
		return 2;
	}
#if !defined(__aarch64__)
	std::fputs("VideoOutGammaTest: this memory-guard suite requires the ARM64 wrapper\n", stderr);
	return 2;
#else
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 1;
	if (open_enabled || open_disabled)
	{
		if (!OpenParameterExperiment(static_cast<size_t>(page_size), open_enabled)) return 1;
		std::puts("VideoOutOpenParamTest: PASS");
		return 0;
	}
	if (!ValuesAndPreservation(static_cast<size_t>(page_size)) ||
	    !MemoryGuards(static_cast<size_t>(page_size)) || !ExactBinding() ||
	    !AdjustLifecycle(static_cast<size_t>(page_size))) return 1;
	std::puts("VideoOutGammaTest: PASS");
	return 0;
#endif
}
