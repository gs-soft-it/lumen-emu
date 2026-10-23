#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace Gen4   = Lumen::Libs::Graphics::Gen4;
namespace Pm4    = Lumen::Libs::Graphics::Pm4;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Loader = Lumen::Loader;

constexpr size_t  REGION_SIZE    = 65536;
constexpr size_t  PACKET_DWORDS  = 40;
constexpr uint8_t CANARY         = 0xb6;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmEmbeddedPsShaderTest: %s\n", message);
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

Loader::SymbolResolve Exact(const char* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = "GraphicsDriver";
	query.library_version      = 1;
	query.module               = "GraphicsDriver";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

bool Missing(const Loader::SymbolDatabase& symbols, const Loader::SymbolResolve& query, const char* mismatch)
{
	if (symbols.Find(query) == nullptr) return true;
	std::fprintf(stderr, "GnmEmbeddedPsShaderTest: unsupported %s resolved\n", mismatch);
	return false;
}

bool Binding(const Loader::SymbolDatabase& symbols, const Loader::HostCallScalarMetadata** metadata)
{
	const auto* symbol = symbols.Find(Exact("X9Omw9dwv5M"));
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Gen4::GraphicsSetEmbeddedPsShader),
	           "exact SetEmbeddedPsShader NID missing")) return false;
	*metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(*metadata != nullptr && (*metadata)->arity == 3 && (*metadata)->result == Loader::HostCallScalarType::Gp &&
	               (*metadata)->invoker != nullptr,
	           "three-argument scalar ABI metadata mismatch")) return false;
	for (size_t index = 0; index < 3; ++index)
		if (!Check((*metadata)->arguments[index] == Loader::HostCallScalarType::Gp,
		           "SetEmbeddedPsShader argument is not a GP scalar")) return false;
	for (const char* name: {"GnmDriver", "libSceGnmDriver"})
	{
		auto        alias    = Exact("X9Omw9dwv5M");
		alias.library        = name;
		alias.module         = name;
		const auto* resolved = symbols.Find(alias);
		if (!Check(resolved != nullptr && resolved->vaddr == symbol->vaddr,
		           "Gnm/Graphics namespace normalization changed")) return false;
	}
	bool passed = true;
	for (int version: {0, 2})
	{
		auto wrong                 = Exact("X9Omw9dwv5M");
		wrong.library_version      = version;
		passed                     = Missing(symbols, wrong, "library version") && passed;
		wrong                      = Exact("X9Omw9dwv5M");
		wrong.module_version_major = version;
		passed                     = Missing(symbols, wrong, "module major version") && passed;
		wrong                      = Exact("X9Omw9dwv5M");
		wrong.module_version_minor = version;
		passed                     = Missing(symbols, wrong, "module minor version") && passed;
	}
	auto wrong    = Exact("X9Omw9dwv5M");
	wrong.library = "unrelated_graphics_library";
	passed        = Missing(symbols, wrong, "library namespace") && passed;
	wrong         = Exact("X9Omw9dwv5M");
	wrong.module  = "unrelated_graphics_library";
	passed        = Missing(symbols, wrong, "module namespace") && passed;
	wrong         = Exact("X9Omw9dwv5M");
	wrong.type    = Loader::SymbolType::Object;
	passed        = Missing(symbols, wrong, "symbol type") && passed;
	if (symbols.FindByStdName(U"X9Omw9dwv5M") != nullptr || symbols.FindByStdName(U"sceGnmSetEmbeddedPsShader") != nullptr ||
	    symbols.StdNameMapSize() != 0)
	{
		std::fputs("GnmEmbeddedPsShaderTest: encoded identity gained a standard-name fallback\n", stderr);
		passed = false;
	}
	return passed;
}

bool PacketCases(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping output;
	if (!Check(output.Valid(), "guest output mapping allocation failed")) return false;
	if (!Check(Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess),
	           "output guard page protection failed")) return false;
	std::array<uint8_t, REGION_SIZE> expected {};
	constexpr std::array<uint32_t, 2> packet {0xc0261038u, 0u};
	if (!Check(packet[0] == LUMEN_PM4(PACKET_DWORDS, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED),
	           "modeled packet header constant changed")) return false;
	for (const size_t offset: {size_t {13}, REGION_SIZE - PACKET_DWORDS * 4})
	{
		for (const uint32_t size: {40u, 41u, 64u, 256u})
		{
			std::memset(output.Data(), CANARY, REGION_SIZE);
			expected.fill(CANARY);
			std::memcpy(expected.data() + offset, packet.data(), sizeof(packet));
			const std::array<uint64_t, 3> arguments {
			    output.Address(offset), UINT64_C(0x89abcdef00000000) | size, UINT64_C(0xffffffff00000000),
			};
			if (!Check(metadata.invoker(arguments.data()) == 0, "scalar call failed with a documented capacity") ||
			    !Check(std::memcmp(output.Data(), expected.data(), REGION_SIZE) == 0,
			           "SetEmbeddedPsShader packet, span or output canary differs")) return false;
		}
	}
	std::memset(output.Data(), CANARY, REGION_SIZE);
	expected.fill(CANARY);
	std::memcpy(expected.data(), packet.data(), sizeof(packet));
	if (!Check(Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data()), 40, 0) == 0,
	           "direct packet builder returned an error") ||
	    !Check(std::memcmp(output.Data(), expected.data(), REGION_SIZE) == 0,
	           "direct SetEmbeddedPsShader packet or output canary differs")) return false;
	return true;
}

bool InvalidCases(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping output;
	if (!Check(output.Valid(), "invalid-case mapping allocation failed")) return false;
	std::array<uint8_t, REGION_SIZE> unchanged {};
	unchanged.fill(CANARY);
	for (const uint32_t size: {0u, 1u, 39u})
	{
		std::memset(output.Data(), CANARY, REGION_SIZE);
		if (!Check(Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data(7)), size, 0) == -1 &&
		               std::memcmp(output.Data(), unchanged.data(), REGION_SIZE) == 0,
		           "insufficient capacity changed the output")) return false;
	}
	for (const int32_t id: {-1, 2, 3, INT32_MAX})
	{
		std::memset(output.Data(), CANARY, REGION_SIZE);
		if (!Check(Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data(7)), 40, id) == -1 &&
		               std::memcmp(output.Data(), unchanged.data(), REGION_SIZE) == 0,
		           "undefined shader ID changed the output")) return false;
	}
	if (!Check(Gen4::GraphicsSetEmbeddedPsShader(nullptr, 40, 0) == -1, "null output pointer was accepted")) return false;
	for (const uint32_t size: {0u, 39u})
		if (!Check(Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(uintptr_t {1}), size, 0) == -1,
		           "invalid argument accessed an unreadable output pointer")) return false;
	const std::array<uint64_t, 3> null_arguments {0, 40, 0};
	if (!Check(metadata.invoker(null_arguments.data()) == UINT64_C(0xffffffff),
	           "null output scalar status mismatch")) return false;
	const std::array<uint64_t, 3> invalid_size_low {output.Address(7), UINT64_C(0x1111111100000027),
	                                                UINT64_C(0x2222222200000000)};
	return Check(metadata.invoker(invalid_size_low.data()) == UINT64_C(0xffffffff) &&
	                 std::memcmp(output.Data(), unchanged.data(), REGION_SIZE) == 0,
	             "scalar call accepted an insufficient low 32-bit capacity or changed output");
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
	if (!FatalCase("undefined shader ID did not fail closed", [&] {
		Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data()), 40, 1);
	})) return false;
	if (!FatalCase("invalid nonnull output did not fail closed", [&] {
		Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(uintptr_t {1}), 40, 0);
	})) return false;
	if (!FatalCase("read-only output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(), REGION_SIZE, Memory::Mode::Read)) std::_Exit(2);
		Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data()), 40, 0);
	})) return false;
	if (!FatalCase("cross-boundary output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess)) std::_Exit(2);
		Gen4::GraphicsSetEmbeddedPsShader(reinterpret_cast<uint32_t*>(output.Data(REGION_SIZE - 4)), 40, 0);
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
	std::puts("GnmEmbeddedPsShaderTest: PASS");
	return 0;
}
