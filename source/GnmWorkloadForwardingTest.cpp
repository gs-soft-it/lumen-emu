#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "AndroidFexCoreRuntime.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
namespace Loader = Lumen::Loader;
namespace Memory = Lumen::Core::VirtualMemory;
constexpr int ORDINARY_RESULT = -1234567;
std::array<uint64_t, 9> g_observed {};
unsigned g_calls = 0;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GnmWorkloadForwardingTest: %s\n", message);
	return condition;
}

// Original synthetic guest calls use distinct values in all GP and spill
// positions. They invoke the real exported wrapper through the FEX registry.
bool Run(uint64_t target, uint64_t token, bool flip)
{
	constexpr std::array<uint64_t, 9> inputs {
		UINT64_C(0xdeadbeef92345678), UINT64_C(0x1234567887654321), UINT64_C(0x23456789abcdef01),
		UINT64_C(0x3456789abcdef012), UINT64_C(0x456789abcdef0123), UINT64_C(0xfeedface81234567),
		UINT64_C(0xfaceb00c76543210), UINT64_C(0xf00dcafe92345678), UINT64_C(0xfedcba9876543210),
	};
	std::vector<uint8_t> code;
	const auto bytes = [&](std::initializer_list<uint8_t> values) { code.insert(code.end(), values); };
	const auto mov = [&](unsigned reg, uint64_t value) {
		bytes({static_cast<uint8_t>(reg < 8 ? 0x48 : 0x49), static_cast<uint8_t>(0xb8 + (reg & 7))});
		for (unsigned shift = 0; shift != 64; shift += 8) code.push_back(static_cast<uint8_t>(value >> shift));
	};
	bytes({0x48, 0x83, 0xec, 40}); // sub rsp,40: four spilled arguments and call alignment
	constexpr unsigned registers[] = {7, 6, 2, 1, 8, 9};
	mov(registers[0], token);
	for (unsigned index = 1; index != 6; ++index) mov(registers[index], inputs[index - 1]);
	for (unsigned index = 0; index != 4; ++index)
	{
		mov(0, inputs[index + 5]);
		bytes({0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(index * 8)}); // mov [rsp+offset],rax
	}
	mov(0, target);
	bytes({0xff, 0xd0, 0x48, 0x83, 0xc4, 40, 0xc3}); // call rax; add rsp,40; ret
	const uint64_t address = Memory::Alloc(0, 65536, Memory::Mode::ReadWrite);
	if (!Check(address != 0, "guest code allocation failed")) return false;
	std::memcpy(reinterpret_cast<void*>(address), code.data(), code.size());
	if (!Memory::Protect(address, 65536, Memory::Mode::ExecuteRead) || !Memory::FlushInstructionCache(address, code.size()))
	{
		Memory::Free(address);
		return Check(false, "guest code publication failed");
	}
	g_calls = 0;
	g_observed = {};
	const uint64_t result = Loader::RunGuestX64(address);
	Memory::Free(address);
	auto expected = inputs;
	expected[0] = static_cast<uint32_t>(expected[0]);
	if (flip)
	{
		for (size_t index = 5; index != 8; ++index) expected[index] = static_cast<uint32_t>(expected[index]);
	} else
	{
		for (size_t index = 5; index != 9; ++index) expected[index] = 0;
	}
	return Check(g_calls == 1 && g_observed == expected, "ordinary submit arguments changed or wrapper skipped dispatch") &&
	       Check(result == static_cast<uint32_t>(ORDINARY_RESULT), "ordinary 32-bit status was not propagated");
}
} // namespace

namespace Lumen::Libs::Graphics::Gen4 {

int LUMEN_SYSV_ABI GnmTestSubmitCommandBuffers(uint32_t count, void* dcb[], const uint32_t* dcb_sizes,
                                              void* ccb[], const uint32_t* ccb_sizes)
{
	++g_calls;
	g_observed = {count, reinterpret_cast<uint64_t>(dcb), reinterpret_cast<uint64_t>(dcb_sizes),
	              reinterpret_cast<uint64_t>(ccb), reinterpret_cast<uint64_t>(ccb_sizes), 0, 0, 0, 0};
	return ORDINARY_RESULT;
}

int LUMEN_SYSV_ABI GnmTestSubmitAndFlipCommandBuffers(uint32_t count, void* dcb[], const uint32_t* dcb_sizes,
                                                     void* ccb[], const uint32_t* ccb_sizes, int handle, int index,
                                                     int mode, int64_t argument)
{
	++g_calls;
	g_observed = {count, reinterpret_cast<uint64_t>(dcb), reinterpret_cast<uint64_t>(dcb_sizes),
	              reinterpret_cast<uint64_t>(ccb), reinterpret_cast<uint64_t>(ccb_sizes),
	              static_cast<uint32_t>(handle), static_cast<uint32_t>(index), static_cast<uint32_t>(mode),
	              static_cast<uint64_t>(argument)};
	return ORDINARY_RESULT;
}
} // namespace Lumen::Libs::Graphics::Gen4

int main()
{
	if (setenv("LUMEN_MACOS_FEXCORE_RUN", "1", 1) != 0) return 1;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	LumenAndroidFexCoreRuntimeStatusV1 status {};
	if (!Check(LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	           status.backendRouteReady != 0, "real FEX runtime unavailable")) return 1;
	auto* linker = Lumen::Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto* symbols = linker->Symbols();
	if (!Check(Lumen::Libs::Init(U"libGraphicsDriver_1", symbols), "real GraphicsDriver registration failed")) return 1;
	const Loader::SymbolResolve submit {U"jRcI8VcgTz4", U"GraphicsDriver", 1, U"GraphicsDriver", 1, 1, Loader::SymbolType::Func};
	const Loader::SymbolResolve flip {U"Ga6r7H6Y0RI", U"GraphicsDriver", 1, U"GraphicsDriver", 1, 1, Loader::SymbolType::Func};
	const auto* submit_symbol = symbols->Find(submit);
	const auto* flip_symbol = symbols->Find(flip);
	if (!Check(submit_symbol != nullptr && flip_symbol != nullptr, "workload NID missing")) return 1;
	const uint64_t submit_target = submit_symbol->vaddr;
	const uint64_t flip_target = flip_symbol->vaddr;
	symbols->SealHostCallRegistry();
	if (!Check(linker->SetupSyntheticTlsForPthreadExitTest(), "synthetic TLS fixture failed")) return 1;
	bool passed = true;
	for (uint64_t token: {UINT64_C(0), UINT64_C(1), UINT64_MAX})
		passed = Run(submit_target, token, false) && Run(flip_target, token, true) && passed;
	linker->Clear();
	if (!passed) return 1;
	std::puts("GnmWorkloadForwardingTest: PASS");
	return 0;
}
