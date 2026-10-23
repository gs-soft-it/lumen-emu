#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = "GraphicsDriver";
	query.library_version      = 1;
	query.module               = "GraphicsDriver";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	return query;
}

bool Missing(const Lumen::Loader::SymbolDatabase& symbols, const Lumen::Loader::SymbolResolve& query, const char* mismatch)
{
	if (symbols.Find(query) == nullptr)
	{
		return true;
	}
	std::fprintf(stderr, "GnmSetCsShaderTest: unsupported %s resolved\n", mismatch);
	return false;
}

bool CheckIdentity(const Lumen::Loader::SymbolDatabase& symbols)
{
	namespace Gen4   = Lumen::Libs::Graphics::Gen4;
	const auto* base = symbols.Find(Exact("KXltnCwEJHQ"));
	if (base == nullptr || base->vaddr != reinterpret_cast<uint64_t>(&Gen4::GraphicsSetCsShader))
	{
		std::fputs("GnmSetCsShaderTest: observed SetCsShader export is missing\n", stderr);
		return false;
	}
	const auto* with_modifier = symbols.Find(Exact("Kx-h-nWQJ8A"));
	if (with_modifier == nullptr || with_modifier->vaddr != reinterpret_cast<uint64_t>(&Gen4::GraphicsSetCsShaderWithModifier))
	{
		std::fputs("GnmSetCsShaderTest: existing SetCsShaderWithModifier binding changed\n", stderr);
		return false;
	}
	for (const char* name: {"GnmDriver", "libSceGnmDriver"})
	{
		auto alias           = Exact("KXltnCwEJHQ");
		alias.library        = name;
		alias.module         = name;
		const auto* resolved = symbols.Find(alias);
		if (resolved == nullptr || resolved->vaddr != base->vaddr)
		{
			std::fputs("GnmSetCsShaderTest: loader Gnm/Graphics namespace normalization changed\n", stderr);
			return false;
		}
	}

	bool passed = true;
	for (int version: {0, 2})
	{
		auto wrong                 = Exact("KXltnCwEJHQ");
		wrong.library_version      = version;
		passed                     = Missing(symbols, wrong, "library version") && passed;
		wrong                      = Exact("KXltnCwEJHQ");
		wrong.module_version_major = version;
		passed                     = Missing(symbols, wrong, "module major version") && passed;
		wrong                      = Exact("KXltnCwEJHQ");
		wrong.module_version_minor = version;
		passed                     = Missing(symbols, wrong, "module minor version") && passed;
	}
	auto wrong    = Exact("KXltnCwEJHQ");
	wrong.library = "unrelated_graphics_library";
	passed        = Missing(symbols, wrong, "library namespace") && passed;
	wrong         = Exact("KXltnCwEJHQ");
	wrong.module  = "unrelated_graphics_library";
	passed        = Missing(symbols, wrong, "module namespace") && passed;
	wrong         = Exact("KXltnCwEJHQ");
	wrong.type    = Lumen::Loader::SymbolType::Object;
	passed        = Missing(symbols, wrong, "symbol type") && passed;
	if (symbols.FindByStdName(U"KXltnCwEJHQ") != nullptr || symbols.FindByStdName(U"sceGnmSetCsShader") != nullptr ||
	    symbols.StdNameMapSize() != 0)
	{
		std::fputs("GnmSetCsShaderTest: encoded identity gained a standard-name fallback\n", stderr);
		passed = false;
	}
	return passed;
}

bool CheckPackets()
{
	namespace Gen4                                  = Lumen::Libs::Graphics::Gen4;
	namespace Pm4                                   = Lumen::Libs::Graphics::Pm4;
	constexpr uint64_t                packet_dwords = 25;
	constexpr uint32_t                canary        = 0xa5c39e71u;
	constexpr std::array<uint32_t, 7> registers     = {
        0x01020304u, 0x11223344u, 0x55667788u, 0x99aabbccu, 0x00000008u, 0x00000004u, 0x00000002u,
    };
	std::array<uint32_t, packet_dwords + 3> base;
	std::array<uint32_t, packet_dwords + 3> modified;
	base.fill(canary);
	modified.fill(canary);

	if (Gen4::GraphicsSetCsShader(base.data(), packet_dwords, registers.data()) != 0 ||
	    Gen4::GraphicsSetCsShaderWithModifier(modified.data(), packet_dwords, registers.data(), 0x7bd135a9u) != 0)
	{
		std::fputs("GnmSetCsShaderTest: packet builder returned an error\n", stderr);
		return false;
	}
	if (base[0] != LUMEN_PM4(packet_dwords, Pm4::IT_NOP, Pm4::R_CS) || base[0] != 0xc017101cu || base[1] != 0 || modified[0] != base[0] ||
	    modified[1] != 0x7bd135a9u)
	{
		std::fputs("GnmSetCsShaderTest: packet header or modifier is incorrect\n", stderr);
		return false;
	}
	for (size_t index = 0; index < registers.size(); ++index)
	{
		if (base[index + 2] != registers[index] || modified[index + 2] != registers[index])
		{
			std::fputs("GnmSetCsShaderTest: compute register payload changed\n", stderr);
			return false;
		}
	}
	for (size_t index = 9; index < base.size(); ++index)
	{
		if (base[index] != canary || modified[index] != canary)
		{
			std::fputs("GnmSetCsShaderTest: packet builder wrote beyond its payload\n", stderr);
			return false;
		}
	}
	return true;
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);

	Lumen::Loader::SymbolDatabase symbols;
	if (!Lumen::Libs::Init(U"libGraphicsDriver_1", &symbols))
	{
		std::fputs("GnmSetCsShaderTest: GraphicsDriver registration failed\n", stderr);
		return 1;
	}

	if (!CheckIdentity(symbols) || !CheckPackets()) return 1;

	std::puts("GnmSetCsShaderTest: PASS");
	return 0;
}
