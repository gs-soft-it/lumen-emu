#include "Lumen/Core/VirtualMemory.h"

#include "CpuX64.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main()
{
	using Lumen::Core::VirtualMemory::Mode;
	using Lumen::Cpu::X64Interpreter;
	using Lumen::Cpu::X64RunResult;
	using Lumen::Cpu::X64State;

	// MOV RAX, 0x1122334455667788; RET
	constexpr std::array<uint8_t, 11> guest_code = {
	    0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xc3,
	};
	constexpr uint64_t expected_result = 0x1122334455667788ull;

	Lumen::Core::VirtualMemory::Init();
	const uint64_t mapping = Lumen::Core::VirtualMemory::Alloc(0, guest_code.size(), Mode::ReadWrite);
	if (mapping == 0)
	{
		std::fputs("apple-bootstrap-smoke: VM allocation failed\n", stderr);
		return 1;
	}

	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)), guest_code.data(), guest_code.size());
	Mode old_mode = Mode::NoAccess;
	bool ok = Lumen::Core::VirtualMemory::Protect(mapping, guest_code.size(), Mode::ExecuteRead, &old_mode) && old_mode == Mode::ReadWrite;

	uint64_t return_address = 0;
	X64State state {};
	state.rip                     = mapping;
	state.gpr[4]                  = reinterpret_cast<uint64_t>(&return_address);
	const X64RunResult run_result = ok ? X64Interpreter {}.Run(&state, 4) : X64RunResult::MemoryFault;
	const bool         cpu_ok     = ok && run_result == X64RunResult::Returned && state.rip == 0 && state.gpr[0] == expected_result;

	old_mode = Mode::NoAccess;
	const bool restored =
	    Lumen::Core::VirtualMemory::Protect(mapping, guest_code.size(), Mode::ReadWrite, &old_mode) && old_mode == Mode::ExecuteRead;
	const bool freed = Lumen::Core::VirtualMemory::Free(mapping);
	const bool vm_ok = ok && restored && freed;

	std::printf("apple-bootstrap-smoke cpu=%s virtual-memory=%s result=0x%016llx\n", cpu_ok ? "ok" : "fail", vm_ok ? "ok" : "fail",
	            static_cast<unsigned long long>(state.gpr[0]));
	return cpu_ok && vm_ok ? 0 : 1;
}
