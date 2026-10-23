#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace Gen4 = Lumen::Libs::Graphics::Gen4;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Loader = Lumen::Loader;

constexpr size_t REGION_SIZE = 65536;
constexpr uint8_t CANARY = 0xa7;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmMarkerTest: %s\n", message);
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

Loader::SymbolResolve MarkerIdentity()
{
	return {U"jiItzS6+22g", U"GraphicsDriver", 1, U"GraphicsDriver", 1, 1, Loader::SymbolType::Func};
}

bool Binding(const Loader::SymbolDatabase& symbols, const Loader::HostCallScalarMetadata** metadata)
{
	const auto* symbol = symbols.Find(MarkerIdentity());
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Gen4::GraphicsInsertSetMarker),
	           "exact Gnm marker NID missing")) return false;
	*metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(*metadata != nullptr && (*metadata)->arity == 3 && (*metadata)->result == Loader::HostCallScalarType::Gp &&
	           (*metadata)->invoker != nullptr, "marker scalar ABI metadata mismatch")) return false;
	for (size_t index = 0; index < 3; ++index)
		if (!Check((*metadata)->arguments[index] == Loader::HostCallScalarType::Gp,
		           "marker argument is not a GP scalar")) return false;
	for (int version: {0, 2})
	{
		auto mismatch = MarkerIdentity();
		mismatch.library_version = version;
		if (!Check(symbols.Find(mismatch) == nullptr, "wrong marker library version resolved")) return false;
		mismatch = MarkerIdentity();
		mismatch.module_version_minor = version;
		if (!Check(symbols.Find(mismatch) == nullptr, "wrong marker module version resolved")) return false;
	}
	return true;
}

struct Case
{
	size_t length;
	uint32_t words;
};

constexpr std::array<Case, 11> CASES {{
	{0, 6}, {1, 6}, {3, 6}, {4, 9}, {7, 9}, {8, 10},
	{11, 10}, {12, 13}, {15, 13}, {16, 14}, {19, 14},
}};

std::string MakeText(size_t length)
{
	std::string result(length, '\0');
	for (size_t index = 0; index < length; ++index)
		result[index] = static_cast<char>('A' + index % 26);
	return result;
}

bool PacketMatches(const Mapping& output, size_t offset, const Case& test, const std::string& text)
{
	std::vector<uint8_t> expected(REGION_SIZE, CANARY);
	const uint32_t header = 0xc0001000u | ((test.words - 2u) << 16u);
	const uint32_t discriminator = 0x68750003u;
	std::memset(expected.data() + offset, 0, 4 * test.words);
	std::memcpy(expected.data() + offset, &header, sizeof(header));
	std::memcpy(expected.data() + offset + 4, &discriminator, sizeof(discriminator));
	if (!text.empty()) std::memcpy(expected.data() + offset + 8, text.data(), text.size());
	return Check(std::memcmp(output.Data(), expected.data(), REGION_SIZE) == 0,
	             "header, discriminator, string, padding or output redzone differs");
}

bool Matrix(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping source, output;
	if (!Check(source.Valid() && output.Valid(), "guest mapping allocation failed")) return false;
	const std::vector<uint8_t> untouched(REGION_SIZE, CANARY);
	for (const Case& test: CASES)
	{
		const std::string text = MakeText(test.length);
		for (const size_t source_offset: {size_t {37}, size_t {4093}})
		{
			std::memset(source.Data(), CANARY, REGION_SIZE);
			std::memcpy(source.Data(source_offset), text.c_str(), text.size() + 1);
			const std::vector<uint8_t> source_before(source.Data(), source.Data() + REGION_SIZE);
			for (const size_t output_offset: {size_t {17}, size_t {4095}, REGION_SIZE - 4 * test.words})
			{
				for (const uint32_t supplied: {test.words - 1, test.words + 1})
				{
					std::memset(output.Data(), CANARY, REGION_SIZE);
					if (!Check(Gen4::GraphicsInsertSetMarker(
					               reinterpret_cast<uint32_t*>(output.Data(output_offset)), supplied,
					               reinterpret_cast<const char*>(source.Data(source_offset))) == -1 &&
					           std::memcmp(output.Data(), untouched.data(), REGION_SIZE) == 0,
					           "N-1 or N+1 changed output")) return false;
				}
				std::memset(output.Data(), CANARY, REGION_SIZE);
				const std::array<uint64_t, 3> arguments {
				    output.Address(output_offset), UINT64_C(0x89abcdef00000000) | test.words,
				    source.Address(source_offset),
				};
				if (!Check(metadata.invoker(arguments.data()) == 0,
				           "scalar bridge did not ignore upper 32 size bits") ||
				    !PacketMatches(output, output_offset, test, text) ||
				    !Check(std::memcmp(source.Data(), source_before.data(), REGION_SIZE) == 0,
				           "marker source was mutated")) return false;
			}
		}
	}
	return true;
}

bool NullCases(const Loader::HostCallScalarMetadata& metadata)
{
	Mapping source, output;
	if (!Check(source.Valid() && output.Valid(), "null-case mapping allocation failed")) return false;
	const std::vector<uint8_t> untouched(REGION_SIZE, CANARY);
	std::memset(output.Data(), CANARY, REGION_SIZE);
	std::memcpy(source.Data(), "test", 5);
	const std::array<uint64_t, 3> null_string {output.Address(), 9, 0};
	if (!Check(Gen4::GraphicsInsertSetMarker(nullptr, 9, reinterpret_cast<const char*>(source.Data())) == -1 &&
	           Gen4::GraphicsInsertSetMarker(nullptr, 9, nullptr) == -1 &&
	           metadata.invoker(null_string.data()) == UINT64_C(0xffffffff) &&
	           std::memcmp(output.Data(), untouched.data(), REGION_SIZE) == 0,
	           "null input status or output preservation mismatch")) return false;
	return true;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
template <typename Action> bool FatalCase(const char* name, Action action)
{
	const pid_t child = fork();
	if (!Check(child >= 0, "fatal-case fork failed")) return false;
	if (child == 0)
	{
		action();
		std::_Exit(2);
	}
	int status = 0;
	if (!Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff), name))
		return false;
	return true;
}

bool GuardCases()
{
	Mapping source, output;
	if (!Check(source.Valid() && output.Valid(), "guard mapping allocation failed")) return false;
	std::memcpy(source.Data(), "test", 5);
	if (!FatalCase("unreadable source did not fail closed", [&] {
		if (!Memory::Protect(source.Address(), REGION_SIZE, Memory::Mode::NoAccess)) std::_Exit(2);
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data()), 9,
		                              reinterpret_cast<const char*>(source.Data()));
	})) return false;
	if (!FatalCase("invalid nonnull source did not fail closed", [&] {
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data()), 0,
		                              reinterpret_cast<const char*>(uintptr_t {1}));
	})) return false;
	std::memset(source.Data(), 'x', 32769);
	if (!FatalCase("unterminated source did not fail closed", [&] {
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data()), 6,
		                              reinterpret_cast<const char*>(source.Data()));
	})) return false;
	std::memset(source.Data(), 'x', 32768);
	source.Data(32768)[0] = 0;
	if (!FatalCase("overlong PM4 packet did not fail closed", [&] {
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data()), 0,
		                              reinterpret_cast<const char*>(source.Data()));
	})) return false;
	std::memcpy(source.Data(), "test", 5);
	if (!FatalCase("invalid nonnull output did not fail closed", [&] {
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(uintptr_t {1}), 9,
		                              reinterpret_cast<const char*>(source.Data()));
	})) return false;
	if (!FatalCase("overlapping source and output did not fail closed", [&] {
		std::memcpy(output.Data(40), "test", 5);
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data(32)), 9,
		                              reinterpret_cast<const char*>(output.Data(40)));
	})) return false;
	if (!FatalCase("read-only output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(), REGION_SIZE, Memory::Mode::Read)) std::_Exit(2);
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data()), 9,
		                              reinterpret_cast<const char*>(source.Data()));
	})) return false;
	if (!FatalCase("cross-boundary output did not fail closed", [&] {
		if (!Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess)) std::_Exit(2);
		Gen4::GraphicsInsertSetMarker(reinterpret_cast<uint32_t*>(output.Data(REGION_SIZE - 18)), 9,
		                              reinterpret_cast<const char*>(source.Data()));
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
	if (!Binding(symbols, &metadata) || !Matrix(*metadata) || !NullCases(*metadata)) return 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (!GuardCases()) return 1;
#endif
	std::puts("GnmMarkerTest: PASS");
	return 0;
}
