#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace Gen4 = Lumen::Libs::Graphics::Gen4;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Loader = Lumen::Loader;

constexpr size_t REGION_SIZE = 65536;
constexpr uint8_t CANARY = 0xb6;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmVgtControlTest: %s\n", message);
	return condition;
}

class Mapping
{
public:
	Mapping(): m_address(Memory::Alloc(0, 2 * REGION_SIZE, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }

private:
	uint64_t m_address;
};

Loader::SymbolResolve Identity()
{
	return {U"cFCp0NX8wf0", U"GraphicsDriver", 1, U"GraphicsDriver", 1, 1, Loader::SymbolType::Func};
}

bool Binding(const Loader::SymbolDatabase& symbols, const Loader::HostCallScalarMetadata** metadata)
{
	const auto* symbol = symbols.Find(Identity());
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Gen4::GraphicsSetVgtControl),
	           "exact SetVgtControl NID missing")) return false;
	*metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(*metadata != nullptr && (*metadata)->arity == 5 && (*metadata)->result == Loader::HostCallScalarType::Gp &&
	           (*metadata)->invoker != nullptr, "five-argument scalar ABI metadata mismatch")) return false;
	for (size_t index = 0; index < 5; ++index)
		if (!Check((*metadata)->arguments[index] == Loader::HostCallScalarType::Gp,
		           "SetVgtControl argument is not a GP scalar")) return false;
	for (int version: {0, 2})
	{
		auto wrong = Identity();
		wrong.library_version = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library version resolved")) return false;
		wrong = Identity();
		wrong.module_version_minor = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module version resolved")) return false;
	}
	return true;
}

bool PacketCases(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping output;
	if (!Check(output.Valid(), "guest output mapping allocation failed")) return false;
	if (!Check(Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess),
	           "output guard page protection failed")) return false;
	std::array<uint8_t, REGION_SIZE> expected {};
	for (const size_t offset: {size_t {13}, REGION_SIZE - 12})
	{
		for (const uint32_t a: {0u, 1u, 255u})
		{
			for (const uint32_t b: {0u, 1u})
			{
				for (const uint32_t c: {0u, 1u})
				{
					std::memset(output.Data(), CANARY, REGION_SIZE);
					expected.fill(CANARY);
					const std::array<uint32_t, 3> packet {0xc0016900u, 0x000002aau, a | (b << 16)};
					std::memcpy(expected.data() + offset, packet.data(), sizeof(packet));
					const std::array<uint64_t, 5> arguments {
					    output.Address(offset), UINT64_C(0x89abcdef00000003),
					    UINT64_C(0x7654321000000000) | a, UINT64_C(0xfedcba9800000000) | b,
					    UINT64_C(0x1234567800000000) | c,
					};
					if (!Check(metadata.invoker(arguments.data()) == 0, "scalar call failed with valid low 32-bit values") ||
					    !Check(std::memcmp(output.Data(), expected.data(), REGION_SIZE) == 0,
					           "SetVgtControl packet or output canary differs")) return false;
				}
			}
		}
	}
	return true;
}

bool InvalidCases(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping output;
	if (!Check(output.Valid(), "invalid-case mapping allocation failed")) return false;
	struct Invalid { uint32_t words; uint32_t a; uint32_t b; uint32_t c; };
	constexpr std::array<Invalid, 10> cases {{
		{0, 0, 0, 0}, {2, 0, 0, 0}, {4, 0, 0, 0}, {UINT32_MAX, 0, 0, 0},
		{3, 256, 0, 0}, {3, UINT32_MAX, 0, 0},
		{3, 0, 2, 0}, {3, 0, UINT32_MAX, 0},
		{3, 0, 0, 2}, {3, 0, 0, UINT32_MAX},
	}};
	std::array<uint8_t, REGION_SIZE> unchanged {};
	unchanged.fill(CANARY);
	for (const Invalid& test: cases)
	{
		std::memset(output.Data(), CANARY, REGION_SIZE);
		if (!Check(Gen4::GraphicsSetVgtControl(reinterpret_cast<uint32_t*>(output.Data(7)),
		                                   test.words, test.a, test.b, test.c) == -1 &&
		           std::memcmp(output.Data(), unchanged.data(), REGION_SIZE) == 0,
		           "invalid argument changed the output")) return false;
		if (!Check(Gen4::GraphicsSetVgtControl(reinterpret_cast<uint32_t*>(uintptr_t {1}),
		                                   test.words, test.a, test.b, test.c) == -1,
		           "invalid argument accessed an unreadable output pointer")) return false;
	}
	if (!Check(Gen4::GraphicsSetVgtControl(nullptr, 3, 0, 0, 0) == -1,
	           "null output pointer was accepted")) return false;
	const std::array<uint64_t, 5> null_arguments {0, 3, 0, 0, 0};
	if (!Check(metadata.invoker(null_arguments.data()) == UINT64_C(0xffffffff),
	           "null output scalar status mismatch")) return false;
	const std::array<uint64_t, 5> invalid_low_a {
	    output.Address(7), UINT64_C(0x1111111100000003), UINT64_C(0x2222222200000100),
	    UINT64_C(0x3333333300000000), UINT64_C(0x4444444400000000),
	};
	return Check(metadata.invoker(invalid_low_a.data()) == UINT64_C(0xffffffff) &&
	                 std::memcmp(output.Data(), unchanged.data(), REGION_SIZE) == 0,
	             "scalar call accepted invalid low 32 bits or changed output");
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
template <typename Action> bool FatalCase(const char* message, Action action)
{
	const pid_t child = fork();
	if (!Check(child >= 0, "fatal-case fork failed")) return false;
	if (child == 0)
	{
		action();
		std::_Exit(2);
	}
	int status = 0;
	return Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	                 WEXITSTATUS(status) == (321 & 0xff), message);
}

bool OutputGuardCases()
{
	Mapping output;
	if (!Check(output.Valid(), "guard mapping allocation failed")) return false;
	if (!FatalCase("invalid nonnull output did not fail closed", [&] {
		Gen4::GraphicsSetVgtControl(reinterpret_cast<uint32_t*>(uintptr_t {1}), 3, 255, 1, 1);
	})) return false;
	if (!FatalCase("read-only output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(), REGION_SIZE, Memory::Mode::Read)) std::_Exit(2);
		Gen4::GraphicsSetVgtControl(reinterpret_cast<uint32_t*>(output.Data()), 3, 255, 1, 1);
	})) return false;
	if (!FatalCase("cross-boundary output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess)) std::_Exit(2);
		Gen4::GraphicsSetVgtControl(reinterpret_cast<uint32_t*>(output.Data(REGION_SIZE - 8)), 3, 255, 1, 1);
	})) return false;
	return true;
}
#endif

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libGraphicsDriver_1", &symbols), "GraphicsDriver registration failed")) return 1;
	const Loader::HostCallScalarMetadata* metadata = nullptr;
	if (!Binding(symbols, &metadata) || !PacketCases(*metadata) || !InvalidCases(*metadata)) return 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (!OutputGuardCases()) return 1;
#endif
	std::puts("GnmVgtControlTest: PASS");
	return 0;
}
