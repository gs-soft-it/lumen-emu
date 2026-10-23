#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "AndroidFexCoreRuntime.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCall.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include "CpuX64.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace Memory = Kernel::Memory;
namespace Vm     = Lumen::Core::VirtualMemory;

using ReserveFunction = int32_t(LUMEN_SYSV_ABI*)(void**, uint64_t, int32_t, uint64_t);
using SetNameFunction = int32_t(LUMEN_SYSV_ABI*)(uint64_t, uint64_t, uint64_t);
using QueryFunction = int32_t(LUMEN_SYSV_ABI*)(uint64_t, int32_t, uint64_t, uint64_t);
using AddressSanitizerQueryFunction = uint32_t(LUMEN_SYSV_ABI*)();

static_assert(std::is_same_v<decltype(&Memory::KernelReserveVirtualRange), ReserveFunction>);
static_assert(std::is_same_v<decltype(&Memory::KernelSetVirtualRangeName), SetNameFunction>);
static_assert(std::is_same_v<decltype(&Memory::KernelVirtualQuery), QueryFunction>);
static_assert(std::is_same_v<decltype(&Memory::KernelIsAddressSanitizerEnabled), AddressSanitizerQueryFunction>);
static_assert(sizeof(Memory::OrbisVirtualQueryInfo) == 72);
static_assert(alignof(Memory::OrbisVirtualQueryInfo) == 8);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, start) == 0x00);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, end) == 0x08);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, offset) == 0x10);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, protection) == 0x18);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, memory_type) == 0x1c);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, category_flags) == 0x20);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, name) == 0x21);
static_assert(offsetof(Memory::OrbisVirtualQueryInfo, reserved) == 0x41);

constexpr uint64_t UNIT = 0x4000;

ReserveFunction                g_reserve       = nullptr;
SetNameFunction                g_set_name      = nullptr;
QueryFunction                  g_query         = nullptr;
Lumen::Loader::SymbolDatabase* g_symbols       = nullptr;
bool                           g_bridge_failed = false;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "KernelVirtualQueryTest: FAIL: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = "libkernel";
	query.library_version      = 1;
	query.module               = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	return query;
}

int32_t InvokeReserve(void** address, uint64_t length, int32_t flags, uint64_t alignment)
{
	Lumen::Cpu::X64State state {};
	state.gpr[7] = reinterpret_cast<uint64_t>(address);
	state.gpr[6] = length;
	state.gpr[2] = static_cast<uint32_t>(flags);
	state.gpr[1] = alignment;
	if (g_symbols == nullptr || g_reserve == nullptr ||
	    !Lumen::Loader::InvokeHostCall(g_symbols, reinterpret_cast<uint64_t>(g_reserve), &state))
	{
		g_bridge_failed = true;
		return (std::numeric_limits<int32_t>::min)();
	}
	return static_cast<int32_t>(static_cast<uint32_t>(state.gpr[0]));
}

int32_t InvokeSetName(uint64_t address, uint64_t length, uint64_t name)
{
	Lumen::Cpu::X64State state {};
	state.gpr[7] = address;
	state.gpr[6] = length;
	state.gpr[2] = name;
	if (g_symbols == nullptr || g_set_name == nullptr ||
	    !Lumen::Loader::InvokeHostCall(g_symbols, reinterpret_cast<uint64_t>(g_set_name), &state))
	{
		g_bridge_failed = true;
		return (std::numeric_limits<int32_t>::min)();
	}
	return static_cast<int32_t>(static_cast<uint32_t>(state.gpr[0]));
}

int32_t InvokeQuery(uint64_t address, int32_t flags, uint64_t info, uint64_t info_size)
{
	Lumen::Cpu::X64State state {};
	state.gpr[7] = address;
	state.gpr[6] = static_cast<uint32_t>(flags);
	state.gpr[2] = info;
	state.gpr[1] = info_size;
	if (g_symbols == nullptr || g_query == nullptr ||
	    !Lumen::Loader::InvokeHostCall(g_symbols, reinterpret_cast<uint64_t>(g_query), &state))
	{
		g_bridge_failed = true;
		return (std::numeric_limits<int32_t>::min)();
	}
	return static_cast<int32_t>(static_cast<uint32_t>(state.gpr[0]));
}

bool CheckScalarMetadata(const Lumen::Loader::SymbolDatabase& symbols, uint64_t target, uint32_t arity)
{
	const auto* metadata = symbols.HostCallScalarMetadataFor(target);
	if (metadata == nullptr || metadata->arity != arity || metadata->result != Lumen::Loader::HostCallScalarType::Gp ||
	    metadata->invoker == nullptr)
	{
		return false;
	}
	for (uint32_t index = 0; index < arity; index++)
	{
		if (metadata->arguments[index] != Lumen::Loader::HostCallScalarType::Gp) return false;
	}
	return true;
}

bool TestBinding(Lumen::Loader::SymbolDatabase* symbols)
{
	if (!Check(symbols != nullptr && Lumen::Libs::Init(U"libkernel_1", symbols), "libkernel registration failed")) return false;

	const auto* reserve = symbols->Find(Exact("7oxv3PPCumo"));
	const auto* set_name = symbols->Find(Exact("DGMG3JshrZU"));
	const auto* query = symbols->Find(Exact("rVjRvHJ0X6c"));
	if (!Check(reserve != nullptr && set_name != nullptr && query != nullptr, "one exact memory binding is absent") ||
	    !Check(reserve->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelReserveVirtualRange) &&
	               set_name->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelSetVirtualRangeName) &&
	               query->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelVirtualQuery),
	           "memory binding target differs") ||
	    !Check(CheckScalarMetadata(*symbols, reserve->vaddr, 4) && CheckScalarMetadata(*symbols, set_name->vaddr, 3) &&
	               CheckScalarMetadata(*symbols, query->vaddr, 4),
	           "scalar metadata differs"))
	{
		return false;
	}
	const auto* checked_release = symbols->Find(Exact("hwVSPCmp5tM"));
	if (!Check(checked_release != nullptr &&
	               checked_release->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelCheckedReleaseDirectMemory),
	           "checked direct-memory release binding is absent"))
	{
		return false;
	}
	g_reserve  = reinterpret_cast<ReserveFunction>(reserve->vaddr);
	g_set_name = reinterpret_cast<SetNameFunction>(set_name->vaddr);
	g_query    = reinterpret_cast<QueryFunction>(query->vaddr);
	g_symbols  = symbols;

	for (const char* nid: {"DGMG3JshrZU", "rVjRvHJ0X6c"})
	{
		for (int version: {0, 2})
		{
			auto wrong            = Exact(nid);
			wrong.library_version = version;
			if (!Check(symbols->Find(wrong) == nullptr, "wrong library version resolved")) return false;
			wrong                      = Exact(nid);
			wrong.module_version_major = version;
			if (!Check(symbols->Find(wrong) == nullptr, "wrong module major resolved")) return false;
			wrong                      = Exact(nid);
			wrong.module_version_minor = version;
			if (!Check(symbols->Find(wrong) == nullptr, "wrong module minor resolved")) return false;
		}
		auto wrong_type = Exact(nid);
		wrong_type.type = Lumen::Loader::SymbolType::Object;
		if (!Check(symbols->Find(wrong_type) == nullptr, "wrong symbol type resolved")) return false;
	}
	return Check(symbols->FindByStdName(U"sceKernelSetVirtualRangeName") == nullptr &&
	                 symbols->FindByStdName(U"sceKernelVirtualQuery") == nullptr,
	             "memory export gained a standard-name fallback");
}

class GuestPages final
{
public:
	GuestPages(): address(Vm::Alloc(0, 4 * UNIT, Vm::Mode::ReadWrite))
	{
		if (address != 0) std::memset(Data(), 0xa5, 4 * UNIT);
	}
	~GuestPages()
	{
		if (address != 0) (void)Vm::Free(address);
	}
	GuestPages(const GuestPages&)            = delete;
	GuestPages& operator=(const GuestPages&) = delete;

	uint8_t* Data() const { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)); }
	uint64_t Address(size_t offset) const { return address + offset; }
	void** Record(size_t offset) const { return reinterpret_cast<void**>(Data() + offset); }

	uint64_t address = 0;
};

bool TestAddressSanitizerCapability(const Lumen::Loader::SymbolDatabase& symbols)
{
	const auto* symbol = symbols.Find(Exact("jh+8XiK4LeE"));
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelIsAddressSanitizerEnabled) &&
	           CheckScalarMetadata(symbols, symbol->vaddr, 0), "guest ASan capability binding or zero-argument ABI differs"))
		return false;
	for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
	{
		auto wrong = Exact("jh+8XiK4LeE");
		switch (mismatch)
		{
			case 0: wrong.library = "libScePosix"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
			case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr, "guest ASan capability resolved with an unsupported identity")) return false;
	}
	Lumen::Cpu::X64State state {};
	for (auto& value: state.gpr) value = UINT64_MAX;
	constexpr int errno_sentinel = 193;
	*Lumen::Libs::Posix::GetErrorAddr() = errno_sentinel;
	const bool invoked = Lumen::Loader::InvokeHostCall(&symbols, symbol->vaddr, &state);
	return Check(invoked && state.gpr[0] == 0 && Memory::KernelIsAddressSanitizerEnabled() == 0 &&
	             *Lumen::Libs::Posix::GetErrorAddr() == errno_sentinel &&
	             symbols.FindByStdName(U"sceKernelIsAddressSanitizerEnabled") == nullptr,
	             "guest ASan capability was enabled, changed errno, read residual arguments or gained a name fallback");
}

void WriteName(const GuestPages& pages, size_t offset, size_t length, char value)
{
	std::memset(pages.Data() + offset, value, length);
	pages.Data()[offset + length] = '\0';
}

Memory::OrbisVirtualQueryInfo ReadInfo(const GuestPages& pages, size_t offset)
{
	Memory::OrbisVirtualQueryInfo info {};
	std::memcpy(&info, pages.Data() + offset, sizeof(info));
	return info;
}

bool NameEquals(const Memory::OrbisVirtualQueryInfo& info, const char* expected)
{
	std::array<char, 33> actual {};
	std::memcpy(actual.data(), info.name, sizeof(info.name));
	return std::strcmp(actual.data(), expected) == 0;
}

bool TestPointerErrors(GuestPages& pages)
{
	constexpr size_t info_offset = 0x800;
	constexpr size_t name_offset = 0x900;
	std::array<uint8_t, sizeof(Memory::OrbisVirtualQueryInfo)> before {};
	std::memcpy(before.data(), pages.Data() + info_offset, before.size());
	WriteName(pages, name_offset, 9, 'u');
	if (!Check(InvokeSetName(pages.address, UNIT, pages.Address(name_offset)) == 0,
	           "unowned range name assignment failed") ||
	    !Check(Memory::GetRangeNameStateForTest().live == 0, "unowned range grew the name registry") ||
	    !Check(InvokeSetName(UNIT, UNIT, 0) == Kernel::KERNEL_ERROR_EFAULT, "null name was accepted") ||
	    !Check(InvokeQuery(UNIT, 0, 0, 72) == Kernel::KERNEL_ERROR_EFAULT, "null query output was accepted") ||
	    !Check(InvokeQuery(1, 0, pages.Address(info_offset), 72) == Kernel::KERNEL_ERROR_EACCES,
	           "unmanaged address did not fail closed") ||
	    !Check(std::memcmp(before.data(), pages.Data() + info_offset, before.size()) == 0,
	           "failed query changed output"))
	{
		return false;
	}

	const uint64_t protected_output = Vm::Alloc(0, 2 * UNIT, Vm::Mode::ReadWrite);
	if (!Check(protected_output != 0, "protected-output fixture allocation failed")) return false;
	const bool read_only_ready = Vm::Protect(protected_output, UNIT, Vm::Mode::Read);
	const int32_t read_only_result = read_only_ready ? InvokeQuery(1, 0, protected_output, 72) : 0;
	const bool restored_first = Vm::Protect(protected_output, UNIT, Vm::Mode::ReadWrite);
	const bool no_access_ready = Vm::Protect(protected_output + UNIT, UNIT, Vm::Mode::NoAccess);
	const int32_t crossing_result = no_access_ready ? InvokeQuery(1, 0, protected_output + UNIT - 36, 72) : 0;
	const bool restored_second = Vm::Protect(protected_output + UNIT, UNIT, Vm::Mode::ReadWrite);
	const bool freed = Vm::Free(protected_output);
	return Check(read_only_ready && restored_first && no_access_ready && restored_second && freed,
	             "protected-output fixture transition failed") &&
	       Check(read_only_result == Kernel::KERNEL_ERROR_EFAULT && crossing_result == Kernel::KERNEL_ERROR_EFAULT,
	             "protected or crossing output was accepted");
}

bool TestFlexibleNamesAndSearch(GuestPages& pages)
{
	constexpr size_t record_offset = 0x100;
	constexpr size_t name_offset   = 0x200;
	constexpr size_t info_offset   = 0x400;
	std::memset(pages.Data() + info_offset - 16, 0x6b, sizeof(Memory::OrbisVirtualQueryInfo) + 32);
	std::memset(pages.Data() + record_offset, 0, sizeof(uint64_t));
	if (!Check(InvokeReserve(pages.Record(record_offset), 3 * UNIT, 0, UNIT) == 0, "parent reservation failed")) return false;
	uint64_t base = 0;
	std::memcpy(&base, pages.Data() + record_offset, sizeof(base));
	void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(base != 0 && Memory::KernelMapFlexibleMemory(&mapped, 2 * UNIT, 3, 0) == 0 &&
	               reinterpret_cast<uint64_t>(mapped) == base,
	           "flexible child mapping failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	if (!Check(InvokeQuery(base - UNIT, 0, pages.Address(info_offset), 72) == Kernel::KERNEL_ERROR_EACCES,
	           "flag zero skipped an unmanaged gap") ||
	    !Check(InvokeQuery(base - UNIT, 2, pages.Address(info_offset), 72) == Kernel::KERNEL_ERROR_EACCES,
	           "non-one flag searched forward") ||
	    !Check(InvokeQuery(base - UNIT, 1, pages.Address(info_offset), 0) == 0,
	           "flag one did not find the next mapping"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	auto info = ReadInfo(pages, info_offset);
	if (!Check(info.start == base && info.end == base + 2 * UNIT && info.offset == 0 && info.protection == 3 &&
	               info.memory_type == 0 && info.category_flags == 0x11 && NameEquals(info, ""),
	           "flexible query fields differ"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	WriteName(pages, name_offset, 5, 'n');
	if (!Check(InvokeSetName(base + 1, UNIT + 1, pages.Address(name_offset)) == 0, "rounded name assignment failed") ||
	    !Check(InvokeQuery(base + UNIT, 0, pages.Address(info_offset), (std::numeric_limits<uint64_t>::max)()) == 0,
	           "named flexible query failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	if (!Check(info.start == base && info.end == base + 2 * UNIT && NameEquals(info, "nnnnn"),
	           "rounded name or query bounds differ") ||
	    !Check(Memory::GetRangeNameStateForTest().live == 1, "single managed name was not normalized"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	WriteName(pages, name_offset, 31, 'q');
	if (!Check(InvokeSetName(base, UNIT, pages.Address(name_offset)) == 0, "31-byte name was rejected"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	WriteName(pages, name_offset, 32, 'x');
	if (!Check(InvokeSetName(base, UNIT, pages.Address(name_offset)) == Kernel::KERNEL_ERROR_ENAMETOOLONG,
	           "32-byte name was accepted") ||
	    !Check(InvokeQuery(base, 0, pages.Address(info_offset), 72) == 0, "query after rejected name failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	if (!Check(info.name[0] == 'q' && info.name[30] == 'q' && info.name[31] == '\0',
	           "rejected name changed the previous 31-byte value") ||
	    !Check(Memory::GetRangeNameStateForTest().live == 2, "split names were not represented exactly"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	pages.Data()[name_offset] = '\0';
	if (!Check(InvokeSetName(base, UNIT, pages.Address(name_offset)) == 0, "empty name did not clear the first run") ||
	    !Check(InvokeQuery(base, 0, pages.Address(info_offset), 72) == 0, "query after empty name failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	if (!Check(info.start == base && info.end == base + UNIT && NameEquals(info, ""),
	           "empty-name split did not preserve the adjacent named run") ||
	    !Check(Memory::GetRangeNameStateForTest().live == 1, "empty name did not compact the registry") ||
	    !Check(InvokeQuery(base + 2 * UNIT, 0, pages.Address(info_offset), 72) == 0, "reserved-hole query failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	return Check(info.start == base + 2 * UNIT && info.end == base + 3 * UNIT && info.protection == 0 &&
	                 info.memory_type == 0 && info.category_flags == 0,
	             "reserved-hole fields differ");
}

bool TestDirectAndLifecycle(GuestPages& pages)
{
	constexpr size_t record_offset = 0x100;
	constexpr size_t name_offset   = 0x200;
	constexpr size_t info_offset   = 0x400;
	uint64_t base = 0;
	std::memcpy(&base, pages.Data() + record_offset, sizeof(base));
	if (!Check(Memory::KernelMunmap(base, 2 * UNIT) == 0, "flexible child unmap failed")) return false;

	int64_t physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 42, &physical) == 0,
	           "direct allocation failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	WriteName(pages, name_offset, 6, 'd');
	if (!Check(Memory::KernelMapNamedDirectMemory(
	               &mapped, UNIT, 3, 0x10, physical, UNIT,
	               reinterpret_cast<const char*>(static_cast<uintptr_t>(pages.Address(name_offset)))) == 0,
	           "named direct child mapping failed") ||
	    !Check(InvokeQuery(base, 0, pages.Address(info_offset), 72) == 0, "direct query failed"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}
	auto info = ReadInfo(pages, info_offset);
	if (!Check(info.start == base && info.end == base + UNIT && info.offset == static_cast<uint64_t>(physical) &&
	               info.protection == 3 && info.memory_type == 42 && info.category_flags == 0x12 && NameEquals(info, "dddddd"),
	           "direct query fields differ") ||
	    !Check(Memory::KernelMprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), UNIT, 0x11) == 0 &&
	               InvokeQuery(base, 0, pages.Address(info_offset), 72) == 0,
	           "protected direct query failed"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	if (!Check(info.protection == 0x11, "whole-range protection was not reported") ||
	    !Check(Memory::KernelMunmap(base, UNIT) == 0, "direct child unmap failed") ||
	    !Check(Memory::KernelReleaseDirectMemory(physical, UNIT) == 0, "direct allocation release failed") ||
	    !Check(Memory::ResetForNextSession(), "session reset failed") ||
	    !Check(Memory::GetRangeNameStateForTest().live == 0 && Memory::GetRangeNameStateForTest().capacity == 0,
	           "session reset retained range-name storage"))
	{
		return false;
	}

	std::memset(pages.Data() + record_offset, 0, sizeof(uint64_t));
	if (!Check(InvokeReserve(pages.Record(record_offset), UNIT, 0, UNIT) == 0, "post-reset reservation failed")) return false;
	std::memcpy(&base, pages.Data() + record_offset, sizeof(base));
	mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapFlexibleMemory(&mapped, UNIT, 3, 0) == 0 &&
	               InvokeQuery(base, 0, pages.Address(info_offset), 72) == 0,
	           "post-reset flexible query failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	info = ReadInfo(pages, info_offset);
	return Check(NameEquals(info, ""), "reset retained a range name") && Check(Memory::ResetForNextSession(), "final reset failed");
}

bool TestCheckedDirectRelease(GuestPages& pages)
{
	struct DirectQueryInfo
	{
		int64_t start;
		int64_t end;
		int32_t memory_type;
		int32_t padding;
	};
	static_assert(sizeof(DirectQueryInfo) == 0x18);

	const auto query_allocated = [](int64_t offset, int64_t start, size_t len)
	{
		DirectQueryInfo info {};
		if (Memory::KernelDirectMemoryQuery(offset, 0, &info, sizeof(info)) != 0) return false;
		return info.start == start && info.end == start + static_cast<int64_t>(len);
	};
	const auto query_absent = [](int64_t offset)
	{
		DirectQueryInfo info {};
		return Memory::KernelDirectMemoryQuery(offset, 0, &info, sizeof(info)) == Kernel::KERNEL_ERROR_EACCES;
	};

	// Console-measured profile (CUH-1216A, fw 12.00, 1 October 2026): the checked
	// release covers whole live allocations (also two contiguous ones in a
	// single call), refuses a boundary mismatch with EINVAL and a start outside
	// every allocation with ENOENT. The unchecked sibling keeps the boundary
	// validation but treats a start outside every allocation as a no-op.
	int64_t first  = -1;
	int64_t second = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 42, &first) == 0 &&
	               Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 42, &second) == 0 &&
	               second == first + static_cast<int64_t>(UNIT),
	           "checked release fixture allocations failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	const int64_t after = second + static_cast<int64_t>(UNIT);
	if (!Check(Memory::KernelCheckedReleaseDirectMemory(first + 0x1000, UNIT - 0x1000) == Kernel::KERNEL_ERROR_EINVAL &&
	               query_allocated(first, first, UNIT),
	           "checked release accepted a boundary-mismatching sub-range") ||
	    !Check(Memory::KernelCheckedReleaseDirectMemory(after, UNIT) == Kernel::KERNEL_ERROR_ENOENT,
	           "checked release did not report ENOENT for an unallocated start") ||
	    !Check(Memory::KernelReleaseDirectMemory(after, UNIT) == 0,
	           "unchecked release of an unallocated start is not a no-op") ||
	    !Check(Memory::KernelCheckedReleaseDirectMemory(first, 2 * UNIT) == 0 && query_absent(first) && query_absent(second),
	           "checked release of two contiguous whole allocations failed") ||
	    !Check(Memory::KernelCheckedReleaseDirectMemory(first, UNIT) == Kernel::KERNEL_ERROR_ENOENT,
	           "second checked release did not report ENOENT") ||
	    !Check(Memory::ResetForNextSession(), "checked release session reset failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	// A mixed multi-owner release is locally unqualified until Core can admit
	// every mapping atomically. Refusal and retry must retain the unmapped
	// prefix, the mapped sibling and its data without any cleanup effects.
	constexpr size_t record_offset = 0x100;
	constexpr size_t info_offset   = 0x400;
	std::memset(pages.Data() + record_offset, 0, sizeof(uint64_t));
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 42, &first) == 0 &&
	               Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 42, &second) == 0 &&
	               second == first + static_cast<int64_t>(UNIT) &&
	               Memory::KernelMapDirectMemory(pages.Record(record_offset), UNIT, 0x33, 0, second, UNIT) == 0,
	           "mixed checked release fixture setup failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	uint64_t mapped = 0;
	std::memcpy(&mapped, pages.Data() + record_offset, sizeof(mapped));
	const uint64_t sentinel = 0x7182a3b4c5d6e7f8;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapped)), &sentinel, sizeof(sentinel));
	const auto generation = Vm::GetMutationGeneration();
	const auto gpu_before = Memory::GetGpuOwnerLifecycleStateForTest();
	for (unsigned attempt = 0; attempt < 2; attempt++)
	{
		Vm::ManagedRegion region {};
		if (!Check(Memory::KernelCheckedReleaseDirectMemory(first, 2 * UNIT) == Kernel::KERNEL_ERROR_EBUSY &&
		               query_allocated(first, first, UNIT) && query_allocated(second, second, UNIT) &&
		               Vm::QueryManagedRegion(mapped, &region) == Vm::ManagedRegionState::Visible &&
		               InvokeQuery(mapped, 0, pages.Address(info_offset), 72) == 0 &&
		               Vm::GetMutationGeneration() == generation,
		           "mixed checked release refusal or retry changed owners or mapping"))
		{
			(void)Memory::ResetForNextSession();
			return false;
		}
		const auto info = ReadInfo(pages, info_offset);
		const auto gpu_after = Memory::GetGpuOwnerLifecycleStateForTest();
		if (!Check(info.start == mapped && info.end == mapped + UNIT && info.offset == static_cast<uint64_t>(second) &&
		               info.protection == 0x33 && info.category_flags == 0x12 &&
		               std::memcmp(reinterpret_cast<const void*>(static_cast<uintptr_t>(mapped)), &sentinel, sizeof(sentinel)) == 0 &&
		               gpu_after.allocations == gpu_before.allocations && gpu_after.waits == gpu_before.waits &&
		               gpu_after.frees == gpu_before.frees,
		           "mixed checked release refusal or retry changed mapping data or GPU lifecycle"))
		{
			(void)Memory::ResetForNextSession();
			return false;
		}
	}
	Vm::ManagedRegion region {};
	if (!Check(Memory::KernelCheckedReleaseDirectMemory(second, UNIT) == 0 && query_absent(second) &&
	               query_allocated(first, first, UNIT) && Vm::QueryManagedRegion(mapped, &region) == Vm::ManagedRegionState::Unmanaged &&
	               Memory::GetGpuOwnerLifecycleStateForTest().frees == gpu_before.frees + 1 &&
	               Memory::KernelCheckedReleaseDirectMemory(first, UNIT) == 0 && query_absent(first) &&
	               Memory::ResetForNextSession(),
	           "single mapped checked release or mixed fixture cleanup failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	return true;
}

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK) && defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
struct QueryLifecycleProbe
{
	std::mutex mutex;
	std::condition_variable ready;
	bool entered = false, release = false, free_output = false, mutation_ok = false;
	uint64_t address = 0, output = 0, mutation = 0;
	int reentry_result = 0;
};

QueryLifecycleProbe* g_query_lifecycle_probe = nullptr;

void QueryAllocationCallback(uintptr_t, size_t)
{
	auto& probe = *g_query_lifecycle_probe;
	std::unique_lock<std::mutex> lock(probe.mutex);
	probe.entered = true;
	probe.ready.notify_all();
	probe.ready.wait(lock, [&] { return probe.release; });
	lock.unlock();
	// The real mapping lifecycle is still owned here. Reentry must refuse,
	// while the Exclusive mutation must progress after the peer yields FEX.
	const int reentry = Memory::KernelVirtualQuery(probe.address, 0, probe.output, 72);
	const bool mutation = probe.free_output ? Vm::Free(probe.mutation)
	                                       : Vm::Protect(probe.mutation, 4 * UNIT, Vm::Mode::Read);
	lock.lock();
	probe.reentry_result = reentry;
	probe.mutation_ok = mutation;
}

void QueryFreeCallback(uintptr_t, size_t) {}

struct QueryLifecycleFexRun
{
	GuestPages code, stack;
	bool hold_lease = false, lease_acquired = false;
	unsigned calls = 0;
	std::atomic<bool> completed {false};
};

uint32_t QueryLifecycleCode(void*, uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) noexcept
{
	constexpr uint64_t guest_page = 4096;
	const uint64_t page = address & ~(guest_page - 1);
	Vm::CodeReadLease lease;
	if (!Vm::TryAcquireCodeReadLease(page, guest_page, &lease)) return 0;
	*base = page; *size = guest_page; *writable = 0;
	Vm::ReleaseCodeReadLease(&lease);
	return 1;
}

uint32_t QueryLifecycleHostCall(void* opaque, uint64_t target, uint64_t* gprs, uint64_t* next) noexcept
{
	auto& fixture = *static_cast<QueryLifecycleFexRun*>(opaque);
	if (target != reinterpret_cast<uint64_t>(g_query)) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	++fixture.calls;
	{
		Vm::GuestDataLease lease;
		if (fixture.hold_lease)
		{
			fixture.lease_acquired = Vm::TryAcquireGuestDataLease(gprs[2], sizeof(Memory::OrbisVirtualQueryInfo), Vm::Mode::Write, &lease);
			if (!fixture.lease_acquired) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
		}
		gprs[0] = static_cast<uint32_t>(InvokeQuery(gprs[7], static_cast<int32_t>(gprs[6]), gprs[2], gprs[1]));
	}
	if (!Lumen::Loader::ReadGuestMemory(gprs[4], next, sizeof(*next))) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	gprs[4] += sizeof(*next);
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool TestQuiescentLifecycleQuery()
{
	for (unsigned sample = 0; sample != 3; ++sample)
	{
		const bool free_output = sample == 1, hold_lease = sample == 2;
		constexpr size_t info_offset = 37, record_offset = 128;
		QueryLifecycleFexRun fixture;
		GuestPages output, reservation_record, owner_record, mutation;
		if (!Check(fixture.code.address != 0 && fixture.stack.address != 0 && output.address != 0 &&
		               reservation_record.address != 0 && owner_record.address != 0 && mutation.address != 0,
		           "quiescent query fixture allocations failed")) return false;
		std::memset(reservation_record.Data() + record_offset, 0, sizeof(uint64_t));
		std::memset(owner_record.Data() + record_offset, 0, sizeof(uint64_t));
		if (!Check(InvokeReserve(reservation_record.Record(record_offset), UNIT, 0, UNIT) == 0,
		           "quiescent query reservation failed")) return false;
		uint64_t reserved = 0;
		std::memcpy(&reserved, reservation_record.Data() + record_offset, sizeof(reserved));
		std::array<uint8_t, 4 * UNIT> before_output;
		std::memcpy(before_output.data(), output.Data(), before_output.size());
		fixture.hold_lease = hold_lease;
		// Original x86-64 leaf: movabs rax,registered Query; call rax; ret.
		std::array<uint8_t, 13> program {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0xc3};
		const uint64_t target = reinterpret_cast<uint64_t>(g_query);
		std::memcpy(program.data() + 2, &target, sizeof(target));
		std::memcpy(fixture.code.Data(), program.data(), program.size());
		if (!Check(Vm::Protect(fixture.code.address, 4 * UNIT, Vm::Mode::ExecuteRead) &&
		               LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1,
		           "quiescent query code publication failed")) return false;
		QueryLifecycleProbe probe;
		probe.free_output = free_output;
		probe.address = reserved;
		probe.output = output.Address(info_offset);
		probe.mutation = free_output ? output.address : mutation.address;
		g_query_lifecycle_probe = &probe;
		if (!Check(Memory::RegisterCallbacks(QueryAllocationCallback, QueryFreeCallback),
		           "quiescent query callback registration failed")) return false;
		const auto before = Memory::GetMemoryLifecycleStateForTest();
		std::atomic<int> map_result {(std::numeric_limits<int>::min)()};
		std::thread owner([&] {
			map_result.store(Memory::KernelMapFlexibleMemory(owner_record.Record(record_offset), UNIT, 3, 0),
			                 std::memory_order_release);
		});
		bool owner_entered;
		{
			std::unique_lock<std::mutex> lock(probe.mutex);
			owner_entered = probe.ready.wait_for(lock, std::chrono::seconds(5), [&] { return probe.entered; });
		}
		// Native/standalone invocations cannot suspend a FEX HLE reader.
		const bool standalone_denied = owner_entered &&
		    InvokeQuery(reserved, 0, probe.output, 72) == Kernel::KERNEL_ERROR_EBUSY &&
		    std::memcmp(output.Data(), before_output.data(), before_output.size()) == 0;
		bool waiting_observed = false;
		std::thread release_owner([&] {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (hold_lease ? fixture.completed.load(std::memory_order_acquire)
				               : Memory::GetMemoryLifecycleStateForTest().waiting != 0)
				{
					waiting_observed = !hold_lease;
					break;
				}
				std::this_thread::yield();
			}
			{
				std::lock_guard<std::mutex> lock(probe.mutex);
				probe.release = true;
			}
			probe.ready.notify_all();
		});
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state); state.abiVersion = 1;
		state.rip = fixture.code.address;
		state.gprs[4] = fixture.stack.address + 4 * UNIT - sizeof(uint64_t);
		state.gprs[7] = reserved; state.gprs[6] = 0;
		state.gprs[2] = probe.output; state.gprs[1] = 72;
		const uint64_t root = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root, sizeof(root));
		const uint32_t run_result = owner_entered
		    ? LumenAndroidFexCoreRunGuestV1(&state, QueryLifecycleHostCall, QueryLifecycleCode, &fixture) : UINT32_MAX;
		fixture.completed.store(true, std::memory_order_release);
		release_owner.join(); owner.join();
		const int expected = hold_lease ? Kernel::KERNEL_ERROR_EBUSY : free_output ? Kernel::KERNEL_ERROR_EFAULT : 0;
		Vm::ManagedRegion region {};
		bool output_correct = false;
		if (free_output && probe.mutation_ok)
		{
			const uint64_t freed = output.address;
			output.address = 0;
			output_correct = Vm::QueryManagedRegion(freed, &region) == Vm::ManagedRegionState::Unmanaged;
		} else if (hold_lease)
		{
			output_correct = std::memcmp(output.Data(), before_output.data(), before_output.size()) == 0;
		} else
		{
			const auto info = ReadInfo(output, info_offset);
			output_correct = info.start == reserved && info.end == reserved + UNIT && info.protection == 0 &&
			    info.offset == 0 && info.memory_type == 0 && info.category_flags == 0 &&
			    std::memcmp(output.Data(), before_output.data(), info_offset) == 0 &&
			    std::memcmp(output.Data() + info_offset + sizeof(info), before_output.data() + info_offset + sizeof(info),
			                before_output.size() - info_offset - sizeof(info)) == 0;
		}
		const auto after = Memory::GetMemoryLifecycleStateForTest();
		const bool cleanup = Memory::ResetForNextSession();
		g_query_lifecycle_probe = nullptr;
		const auto final_state = Memory::GetMemoryLifecycleStateForTest();
		const bool passed = owner_entered && standalone_denied && map_result.load(std::memory_order_acquire) == 0 &&
		    probe.mutation_ok && probe.reentry_result == Kernel::KERNEL_ERROR_EBUSY && (hold_lease || waiting_observed) &&
		    run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && fixture.calls == 1 &&
		    (!hold_lease || fixture.lease_acquired) && static_cast<int32_t>(state.gprs[0]) == expected &&
		    state.rip == root && state.gprs[4] == fixture.stack.address + 4 * UNIT && output_correct &&
		    after.try_denials == before.try_denials + 2 + (hold_lease ? 1 : 0) && cleanup &&
		    final_state.active == 0 && final_state.waiting == 0;
		std::printf("KernelVirtualQueryTest: quiescent-query case=%s waiting=%u mutation=%u run=%u result=0x%08x pass=%u\n",
		    hold_lease ? "held-lease-denied" : free_output ? "output-unmapped" : "contended-success",
		    waiting_observed, probe.mutation_ok, run_result, static_cast<uint32_t>(state.gprs[0]), passed);
		if (!Check(passed, "quiescent query result or lifecycle cleanup differs")) return false;
	}
	return true;
}
#endif

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2 || (std::strcmp(argv[1], "--snapshot-enabled") != 0 && std::strcmp(argv[1], "--snapshot-disabled") != 0))
	{
		std::fprintf(stderr, "usage: KernelVirtualQueryTest --snapshot-enabled|--snapshot-disabled\n");
		return 2;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::MemorySubsystem::Instance()->Init(nullptr);
	const bool expected_snapshot = std::strcmp(argv[1], "--snapshot-enabled") == 0;

	Lumen::Loader::SymbolDatabase symbols;
	GuestPages                    pages;
	Memory::SetGpuOwnerLifecycleProbeForTest(true);
	bool ok = Check(Vm::IsImmutableRegionSnapshotEnabled() == expected_snapshot, "snapshot selector oracle differs") &&
	                Check(pages.address != 0, "guest fixture allocation failed") && TestBinding(&symbols) &&
	                TestAddressSanitizerCapability(symbols) && TestPointerErrors(pages) &&
	                TestFlexibleNamesAndSearch(pages) && TestDirectAndLifecycle(pages) && TestCheckedDirectRelease(pages) &&
	                Check(!g_bridge_failed, "scalar host-call bridge failed");
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK) && defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	ok = ok && TestQuiescentLifecycleQuery() && Check(!g_bridge_failed, "quiescent query scalar bridge failed");
#endif
	Memory::SetGpuOwnerLifecycleProbeForTest(false);
	if (!ok) return 1;
	std::puts("KernelVirtualQueryTest: PASS");
	return 0;
}
