#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "CpuX64.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <vector>

namespace Lumen::Loader {
extern thread_local bool (*g_host_call_guest_memory_read)(Cpu::X64State*, uint64_t, size_t, void*, size_t);
}

namespace {
namespace Video = Lumen::Libs::VideoOut;
namespace Memory = Lumen::Core::VirtualMemory;
constexpr uint32_t LOCAL_BUDGET = 64 * 1024;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "VideoOutModeInitTest: %s\n", message);
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

std::vector<uint8_t> Sentinels(size_t size)
{
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i != size; ++i) bytes[i] = static_cast<uint8_t>(11 + i * 37);
	return bytes;
}

bool InitializationBytes(size_t page_size)
{
	const size_t capacity = ((LOCAL_BUDGET + 63 + page_size - 1) / page_size) * page_size;
	Mapping mapping(capacity);
	if (!Check(mapping.Valid(), "byte fixture allocation failed")) return false;
	const auto original = Sentinels(capacity);
	for (uint32_t size: {4u, 5u, 32u, 73u, LOCAL_BUDGET})
		for (size_t offset: {size_t(16), size_t(19)})
		{
			std::memcpy(mapping.Data(), original.data(), original.size());
			auto expected = original;
			std::memset(expected.data() + offset, 0xff, size);
			std::memcpy(expected.data() + offset, &size, sizeof(size));
			if (!Check(Video::VideoOutTryInitializeMode(mapping.Data(offset), size),
			           "valid size or unaligned destination was rejected") ||
			    !Check(std::memcmp(mapping.Data(), expected.data(), capacity) == 0,
			           "initializer bytes or surrounding sentinels were wrong")) return false;
		}
	std::memcpy(mapping.Data(), original.data(), original.size());
	for (uint32_t size: {0u, 1u, 2u, 3u, LOCAL_BUDGET + 1, std::numeric_limits<uint32_t>::max()})
		if (!Check(!Video::VideoOutTryInitializeMode(mapping.Data(19), size),
		           "initializer accepted a size outside its local budget") ||
		    !Check(std::memcmp(mapping.Data(), original.data(), capacity) == 0,
		           "rejected size changed destination bytes")) return false;
	return true;
}

bool MemoryGuards(size_t page_size)
{
	Mapping mapping(page_size * 2);
	if (!Check(mapping.Valid(), "guard fixture allocation failed")) return false;
	const auto original = Sentinels(page_size * 2);
	std::memcpy(mapping.Data(), original.data(), original.size());
	if (!Check(mapping.Protect(0, page_size * 2, Memory::Mode::Read), "read-only protection failed") ||
	    !Check(!Video::VideoOutTryInitializeMode(mapping.Data(19), 32), "initializer wrote to read-only memory") ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0, "read-only rejection changed bytes") ||
	    !Check(mapping.Protect(0, page_size, Memory::Mode::ReadWrite) &&
	               mapping.Protect(page_size, page_size, Memory::Mode::NoAccess), "guard page protection failed")) return false;
	for (uint32_t prefix: {1u, 3u, 4u, 31u})
		if (!Check(!Video::VideoOutTryInitializeMode(mapping.Data(page_size - prefix), 32),
		           "initializer accepted a partly writable range") ||
		    !Check(std::memcmp(mapping.Data(), original.data(), page_size) == 0,
		           "range rejection published a partial initializer")) return false;
	if (!Check(!Video::VideoOutTryInitializeMode(mapping.Data(page_size), 4), "initializer accepted a no-access range") ||
	    !Check(mapping.Protect(0, page_size * 2, Memory::Mode::ReadWrite), "guard protection restore failed") ||
	    !Check(std::memcmp(mapping.Data(), original.data(), original.size()) == 0, "guarded rejection changed bytes")) return false;
	auto expected = original;
	const uint32_t minimum_size = 4;
	std::memcpy(expected.data() + page_size - minimum_size, &minimum_size, sizeof(minimum_size));
	if (!Check(mapping.Protect(page_size, page_size, Memory::Mode::NoAccess), "boundary guard protection failed") ||
	    !Check(Video::VideoOutTryInitializeMode(mapping.Data(page_size - minimum_size), minimum_size),
	           "initializer required bytes beyond its requested range") ||
	    !Check(mapping.Protect(page_size, page_size, Memory::Mode::ReadWrite), "boundary guard restore failed") ||
	    !Check(std::memcmp(mapping.Data(), expected.data(), expected.size()) == 0,
	           "boundary initialization changed sentinels")) return false;
	const uint64_t freed = mapping.Address();
	if (!Check(mapping.Free(), "guard fixture free failed")) return false;
	for (uint64_t address: {uint64_t(0), uint64_t(1), freed, std::numeric_limits<uint64_t>::max() - 2})
		if (!Check(!Video::VideoOutTryInitializeMode(reinterpret_cast<void*>(address), 32),
		           "initializer accepted null, unmapped or overflowing output")) return false;
	return true;
}

thread_local unsigned g_stack_reads = 0;

class RejectStackReads
{
public:
	RejectStackReads(): m_previous(Lumen::Loader::g_host_call_guest_memory_read)
	{
		g_stack_reads = 0;
		Lumen::Loader::g_host_call_guest_memory_read = [](Lumen::Cpu::X64State* state, uint64_t base,
		                                               size_t offset, void*, size_t size)
		{
			++g_stack_reads;
			state->memory_fault_address = base <= UINT64_MAX - offset ? base + offset : base;
			state->memory_fault_size = static_cast<uint32_t>(size);
			return false;
		};
	}
	~RejectStackReads() { Lumen::Loader::g_host_call_guest_memory_read = m_previous; }
	RejectStackReads(const RejectStackReads&) = delete;
	RejectStackReads& operator=(const RejectStackReads&) = delete;
private:
	decltype(Lumen::Loader::g_host_call_guest_memory_read) m_previous;
};

bool RegisteredInvocation(Lumen::Loader::SymbolDatabase* symbols, size_t page_size)
{
	using namespace Lumen::Loader;
	Mapping output(page_size);
	Mapping stack(page_size * 2);
	if (!Check(output.Valid() && stack.Valid() && stack.Protect(page_size, page_size, Memory::Mode::NoAccess),
	           "registered-call fixture allocation failed")) return false;
	const auto original = Sentinels(page_size);
	std::memcpy(output.Data(), original.data(), original.size());
	auto expected = original;
	const uint32_t size = 5;
	std::memcpy(expected.data() + 19, &size, sizeof(size));
	expected[23] = 0xff;
	const uint64_t target = reinterpret_cast<uint64_t>(&Video::VideoOutModeSetAny);
	const auto* metadata = symbols->HostCallScalarMetadataFor(target);
	if (!Check(metadata != nullptr && metadata->arity == 2 && metadata->invoker != nullptr &&
	               metadata->arguments[0] == HostCallScalarType::Gp && metadata->arguments[1] == HostCallScalarType::Gp &&
	               metadata->result == HostCallScalarType::Void, "initializer lacks its void two-GP scalar ABI")) return false;
	symbols->SealHostCallRegistry();
	RejectStackReads reader;
	Lumen::Cpu::X64State state {Lumen::Cpu::X64State::HostCallOnly {}};
	state.gpr[0] = UINT64_C(0x6bacdfe012345789);
	state.gpr[4] = stack.Address() + page_size - sizeof(uint64_t);
	state.gpr[7] = output.Address() + 19;
	state.gpr[6] = UINT64_C(0x1234567800000000) | size;
	uint64_t registers[16];
	std::memcpy(registers, state.gpr, sizeof(registers));
	if (!Check(InvokeHostCall(symbols, target, &state) && g_stack_reads == 0 && state.memory_fault_size == 0,
	           "registered initializer read unused stack arguments") ||
	    !Check(std::memcmp(registers, state.gpr, sizeof(registers)) == 0,
	           "void initializer changed RAX or its argument registers") ||
	    !Check(std::memcmp(output.Data(), expected.data(), expected.size()) == 0,
	           "registered initializer did not truncate uint32 size or publish the exact output")) return false;
	return true;
}

bool SelectorAndBinding(bool enabled, size_t page_size)
{
	using namespace Lumen::Loader;
	const SymbolResolve request {U"pjkDsgxli6c", U"VideoOut", 1, U"VideoOut", 0, 0, SymbolType::Func};
	const uint64_t target = reinterpret_cast<uint64_t>(&Video::VideoOutModeSetAny);
	SymbolDatabase first;
	if (!Check(Lumen::Libs::Init(U"libVideoOut_1", &first), "first registry initialization failed")) return false;
	const auto check_binding = [&](const SymbolDatabase& symbols)
	{
		const auto* record = symbols.Find(request);
		return Check(enabled ? record != nullptr && record->vaddr == target : record == nullptr,
		             "mode initializer binding disagrees with the process selector") &&
		       Check((symbols.HostCallScalarMetadataFor(target) != nullptr) == enabled,
		             "disabled initializer unexpectedly has scalar metadata") &&
		       Check(symbols.FindByStdName(U"sceVideoOutModeSetAny_") == nullptr,
		             "initializer acquired a standard-name fallback");
	};
	if (!check_binding(first) ||
	    !Check(setenv("LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT", enabled ? "0" : "1", 1) == 0,
	           "selector environment mutation failed")) return false;
	SymbolDatabase second;
	if (!Check(Lumen::Libs::Init(U"libVideoOut_1", &second), "second registry initialization failed") ||
	    !check_binding(second)) return false;
	for (unsigned mismatch = 0; mismatch != 7; ++mismatch)
	{
		auto other = request;
		switch (mismatch)
		{
			case 0: other.name = U"pjkDsgxli6d"; break;
			case 1: other.library = U"VideoOutOther"; break;
			case 2: other.library_version = 2; break;
			case 3: other.module = U"VideoOutOther"; break;
			case 4: other.module_version_major = 1; break;
			case 5: other.module_version_minor = 1; break;
			case 6: other.type = SymbolType::Object; break;
		}
		if (!Check(second.Find(other) == nullptr, "initializer bound through a mismatched identity")) return false;
	}
	return !enabled || RegisteredInvocation(&second, page_size);
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fputs("VideoOutModeInitTest: expected --memory, --enabled or --disabled\n", stderr);
		return 2;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 1;
	bool passed = false;
	if (std::strcmp(argv[1], "--memory") == 0)
		passed = InitializationBytes(static_cast<size_t>(page_size)) && MemoryGuards(static_cast<size_t>(page_size));
	else if (std::strcmp(argv[1], "--enabled") == 0 || std::strcmp(argv[1], "--disabled") == 0)
		passed = SelectorAndBinding(std::strcmp(argv[1], "--enabled") == 0, static_cast<size_t>(page_size));
	else return 2;
	if (!passed) return 1;
	std::puts("VideoOutModeInitTest: PASS");
	return 0;
}
