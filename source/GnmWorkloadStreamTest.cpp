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
#include <cstring>
#include <string>

namespace {
namespace Gnm = Lumen::Libs::Graphics::Gen4;
namespace Memory = Lumen::Core::VirtualMemory;
namespace Loader = Lumen::Loader;
constexpr size_t REGION_SIZE = 65536;
constexpr uint8_t CANARY = 0xa7;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmWorkloadStreamTest: %s\n", message);
	return condition;
}

class Mapping
{
public:
	Mapping(): m_address(Memory::Alloc(0, 2 * REGION_SIZE, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }
	bool Valid() const { return m_address != 0; }
private:
	uint64_t m_address;
};

Loader::SymbolResolve Exact(const char* nid)
{
	Loader::SymbolResolve result {U"", U"GraphicsDriver", 1, U"GraphicsDriver", 1, 1, Loader::SymbolType::Func};
	result.name = nid;
	return result;
}

bool Bindings(const Loader::SymbolDatabase& symbols)
{
	using Type = Loader::HostCallScalarType;
	struct Expected { const char* nid; uint64_t target; uint32_t arity; Type result; };
	const std::array<Expected, 7> expected {{
		{"5udAm+6boVg", reinterpret_cast<uint64_t>(&Gnm::GraphicsCreateWorkloadStream), 2, Type::Gp},
		{"UtObDRQiGbs", reinterpret_cast<uint64_t>(&Gnm::GraphicsDestroyWorkloadStream), 1, Type::Gp},
		{"ihxrbsoSKWc", reinterpret_cast<uint64_t>(&Gnm::GraphicsBeginWorkload), 2, Type::Gp},
		{"Fa3x75OOLRA", reinterpret_cast<uint64_t>(&Gnm::GraphicsEndWorkload), 1, Type::Gp},
		{"jRcI8VcgTz4", reinterpret_cast<uint64_t>(&Gnm::GraphicsSubmitCommandBuffersForWorkload), 6, Type::Gp},
		{"Ga6r7H6Y0RI", reinterpret_cast<uint64_t>(&Gnm::GraphicsSubmitAndFlipCommandBuffersForWorkload), 10, Type::Gp},
		{"byXlqupd8cE", reinterpret_cast<uint64_t>(&Gnm::GraphicsDingDong), 2, Type::Void},
	}};
	for (const auto& entry: expected)
	{
		const auto* symbol = symbols.Find(Exact(entry.nid));
		if (!Check(symbol != nullptr && symbol->vaddr == entry.target, "NID identity mismatch")) return false;
		const auto* metadata = symbols.HostCallScalarMetadataFor(entry.target);
		if (!Check(metadata != nullptr && metadata->arity == entry.arity && metadata->invoker != nullptr &&
		           metadata->result == entry.result, "fixed scalar signature missing")) return false;
		for (uint32_t index = 0; index < entry.arity; ++index)
			if (!Check(metadata->arguments[index] == Loader::HostCallScalarType::Gp, "non-GP argument metadata")) return false;
		auto mismatch = Exact(entry.nid);
		mismatch.library_version = 2;
		if (!Check(symbols.Find(mismatch) == nullptr, "wrong library version resolved")) return false;
		mismatch = Exact(entry.nid);
		mismatch.module_version_minor = 2;
		if (!Check(symbols.Find(mismatch) == nullptr, "wrong module version resolved")) return false;
	}
	return Check(symbols.Find(Exact("6YRHhh5mHCs")) == nullptr, "unqualified RequestFlipAndSubmitDone resolved");
}

template <typename T> bool OutputMatches(const Mapping& mapping, size_t offset, T value)
{
	std::array<uint8_t, sizeof(T)> bytes {};
	std::memcpy(bytes.data(), &value, sizeof(value));
	for (size_t index = 0; index < REGION_SIZE; ++index)
	{
		const uint8_t expected = index >= offset && index - offset < bytes.size() ? bytes[index - offset] : CANARY;
		if (mapping.Data()[index] != expected) return Check(false, "output value, width or surrounding canary changed");
	}
	return true;
}

bool Contract(const Loader::SymbolDatabase& symbols)
{
	// The native zero queue does not access graphics state. No GPU is
	// initialized in this test, so both registered names must return here.
	for (const char* nid: {"bX5IbRvECXk", "byXlqupd8cE"})
	{
		const auto* symbol = symbols.Find(Exact(nid));
		if (!Check(symbol != nullptr, "DingDong identity missing")) return false;
		const auto* metadata = symbols.HostCallScalarMetadataFor(symbol->vaddr);
		if (!Check(metadata != nullptr && metadata->arity == 2 && metadata->result == Loader::HostCallScalarType::Void,
		           "DingDong typed void signature missing")) return false;
		for (uint64_t offset: {UINT64_C(0), UINT64_C(0xffffffff)})
		{
			std::array<uint64_t, 2> arguments {0, offset};
			metadata->invoker(arguments.data());
		}
	}
	Mapping output;
	if (!Check(output.Valid(), "output allocation failed") ||
	    !Check(Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess), "guard setup failed")) return false;
	const auto* unreadable_name = reinterpret_cast<const char*>(uintptr_t {1});
	std::memset(output.Data(), CANARY, REGION_SIZE);
	if (!Check(Gnm::GraphicsCreateWorkloadStream(nullptr, reinterpret_cast<uint32_t*>(output.Data())) == 3 &&
	           Gnm::GraphicsCreateWorkloadStream(unreadable_name, nullptr) == 3 &&
	           Gnm::GraphicsCreateWorkloadStream(nullptr, nullptr) == 3 &&
	           Gnm::GraphicsCreateWorkloadStream(nullptr, reinterpret_cast<uint32_t*>(uintptr_t {1})) == 3,
	           "Create null error or precedence mismatch") ||
	    !OutputMatches(output, 0, uint32_t {0xa7a7a7a7})) return false;
	const auto* create = symbols.HostCallScalarMetadataFor(reinterpret_cast<uint64_t>(&Gnm::GraphicsCreateWorkloadStream));
	for (size_t offset: {size_t {17}, REGION_SIZE - sizeof(uint32_t)})
	{
		std::memset(output.Data(), CANARY, REGION_SIZE);
		std::array<uint64_t, 2> arguments {1, output.Address(offset)};
		if (!Check(create->invoker(arguments.data()) == 0, "Create rejected unreadable nonnull name") ||
		    !OutputMatches(output, offset, uint32_t {1})) return false;
	}
	for (uint32_t stream = 0; stream <= 17; ++stream)
	{
		const uint32_t destroy_result = stream >= 1 && stream <= 15 ? 0 : 1;
		if (!Check(Gnm::GraphicsDestroyWorkloadStream(stream) == destroy_result &&
		           Gnm::GraphicsDestroyWorkloadStream(stream) == destroy_result,
		           "Destroy boundary or repeat mismatch")) return false;
	}
	if (!Check(Gnm::GraphicsDestroyWorkloadStream(UINT32_MAX) == 1, "Destroy maximum stream accepted")) return false;
	const auto* begin = symbols.HostCallScalarMetadataFor(reinterpret_cast<uint64_t>(&Gnm::GraphicsBeginWorkload));
	for (uint32_t stream: {0u, 1u, 15u, 16u, 17u, UINT32_MAX})
	{
		if (!Check(Gnm::GraphicsBeginWorkload(stream, nullptr) == 3, "Begin null output precedence mismatch")) return false;
		for (size_t offset: {size_t {19}, REGION_SIZE - sizeof(uint64_t)})
		{
			std::memset(output.Data(), CANARY, REGION_SIZE);
			// Only the declared low 32 bits of the stream argument are consumed.
			std::array<uint64_t, 2> arguments {UINT64_C(0x1234567800000000) | stream, output.Address(offset)};
			if (!Check(begin->invoker(arguments.data()) == (stream < 16 ? 0u : 1u), "Begin return mismatch") ||
			    !OutputMatches(output, offset, uint64_t {stream < 16 ? 1u : 0u})) return false;
		}
	}
	const auto* end = symbols.HostCallScalarMetadataFor(reinterpret_cast<uint64_t>(&Gnm::GraphicsEndWorkload));
	for (uint64_t workload: {UINT64_C(0), UINT64_C(1), UINT64_C(0x00ffffffffffffff),
	                        UINT64_C(0x0100000000000000), UINT64_C(0x0f00000000000000),
	                        UINT64_C(0x0fffffffffffffff), UINT64_C(0x1000000000000000),
	                        UINT64_C(0x1000000000000001), UINT64_MAX})
	{
		const uint64_t expected = workload == 0 || workload >= UINT64_C(0x1000000000000000) ? 2 : 0;
		if (!Check(end->invoker(&workload) == expected, "End 64-bit boundary mismatch")) return false;
	}
	return true;
}

int RejectedOutput(const std::string& mode)
{
	const bool create = mode.rfind("create-", 0) == 0;
	const bool begin = mode.rfind("begin-", 0) == 0;
	if (!create && !begin) return 2;
	const std::string guard = mode.substr(mode.find('-') + 1);
	const size_t width = create ? sizeof(uint32_t) : sizeof(uint64_t);
	Mapping output;
	if (!output.Valid()) return 2;
	uint64_t address = output.Address();
	if (guard == "invalid") address = 1;
	else if (guard == "overflow") address = UINT64_MAX - (width - 2);
	else if (guard == "readonly" || guard == "noaccess")
	{
		if (!Memory::Protect(address, REGION_SIZE, guard == "readonly" ? Memory::Mode::Read : Memory::Mode::NoAccess)) return 2;
	} else if (guard == "crosspage")
	{
		if (!Memory::Protect(output.Address(REGION_SIZE), REGION_SIZE, Memory::Mode::NoAccess)) return 2;
		address = output.Address(REGION_SIZE - width / 2);
	} else return 2;
	if (create) Gnm::GraphicsCreateWorkloadStream(reinterpret_cast<const char*>(uintptr_t {1}), reinterpret_cast<uint32_t*>(address));
	else Gnm::GraphicsBeginWorkload(1, reinterpret_cast<uint64_t*>(address));
	std::fputs("GnmWorkloadStreamTest: invalid output returned to caller\n", stderr);
	return 2;
}
} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	if (argc == 2) return RejectedOutput(argv[1]);
	if (argc != 1) return 2;
	Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libGraphicsDriver_1", &symbols), "GraphicsDriver registration failed") ||
	    !Bindings(symbols) || !Contract(symbols)) return 1;
	std::puts("GnmWorkloadStreamTest: PASS");
	return 0;
}
