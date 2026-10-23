#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "AndroidFexCoreRuntime.h"
#include "cpu/runtime/MemoryHandoff.h"

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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
static_assert(std::is_same_v<decltype(&Memory::KernelReserveVirtualRange), ReserveFunction>);
static_assert(sizeof(void*) == 8);

constexpr uint64_t             UNIT            = 0x4000;
ReserveFunction                g_reserve       = nullptr;
Lumen::Loader::SymbolDatabase* g_symbols       = nullptr;
bool                           g_bridge_failed = false;

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK) && defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
struct CallbackProbe
{
	std::mutex              mutex;
	std::condition_variable ready;
	bool                    block_alloc   = false;
	bool                    block_free    = false;
	bool                    alloc_entered = false;
	bool                    free_entered  = false;
	uint64_t                alloc_calls   = 0;
	uint64_t                free_calls    = 0;
	uintptr_t               alloc_address = 0;
	uintptr_t               free_address  = 0;
	size_t                  alloc_size    = 0;
	size_t                  free_size     = 0;
	bool                    mutate_after_alloc = false;
	bool                    free_during_alloc = false;
	uint64_t                mutation_address = 0;
	uint64_t                mutation_size = 0;
	void**                  reentry_record = nullptr;
	int                     reentry_result = 0;
	bool                    mutation_ok = false;
};

CallbackProbe g_callback_probe;

void AllocationCallback(uintptr_t address, size_t size)
{
	std::unique_lock<std::mutex> lock(g_callback_probe.mutex);
	g_callback_probe.alloc_calls++;
	g_callback_probe.alloc_address = address;
	g_callback_probe.alloc_size    = size;
	g_callback_probe.alloc_entered = true;
	g_callback_probe.ready.notify_all();
	g_callback_probe.ready.wait(lock, [] { return !g_callback_probe.block_alloc; });
	const bool mutate = g_callback_probe.mutate_after_alloc;
	const bool release = g_callback_probe.free_during_alloc;
	const uint64_t mutation_address = g_callback_probe.mutation_address;
	const uint64_t mutation_size = g_callback_probe.mutation_size;
	void** const reentry_record = g_callback_probe.reentry_record;
	lock.unlock();
	if (mutate)
	{
		// The real lifecycle remains owned by KernelMapFlexibleMemory here.
		// Its callback must reject same-thread reentry, and its Exclusive VM
		// operation must progress while a peer reservation waits quiescently.
		const int reentry_result = Memory::KernelReserveVirtualRange(reentry_record, UNIT, 0, UNIT);
		const bool mutation_ok = release ? Vm::Free(mutation_address)
		                                 : Vm::Protect(mutation_address, mutation_size, Vm::Mode::Read);
		std::lock_guard<std::mutex> result_lock(g_callback_probe.mutex);
		g_callback_probe.reentry_result = reentry_result;
		g_callback_probe.mutation_ok = mutation_ok;
	}
}

void FreeCallback(uintptr_t address, size_t size)
{
	std::unique_lock<std::mutex> lock(g_callback_probe.mutex);
	g_callback_probe.free_calls++;
	g_callback_probe.free_address = address;
	g_callback_probe.free_size    = size;
	g_callback_probe.free_entered = true;
	g_callback_probe.ready.notify_all();
	g_callback_probe.ready.wait(lock, [] { return !g_callback_probe.block_free; });
}

void UnusedCallback(uintptr_t, size_t) {}

bool WaitForCallback(bool allocation)
{
	std::unique_lock<std::mutex> lock(g_callback_probe.mutex);
	return g_callback_probe.ready.wait_for(lock, std::chrono::seconds(5), [allocation]
	                                       { return allocation ? g_callback_probe.alloc_entered : g_callback_probe.free_entered; });
}

bool WaitForResult(const std::atomic<int>& result, int pending)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do
	{
		if (result.load(std::memory_order_acquire) != pending) return true;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

void ReleaseCallbackBlocks()
{
	{
		std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
		g_callback_probe.block_alloc = false;
		g_callback_probe.block_free  = false;
	}
	g_callback_probe.ready.notify_all();
}
#endif

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "KernelVirtualRangeTest: FAIL: %s\n", message);
	}
	return condition;
}

Lumen::Loader::SymbolResolve Exact()
{
	Lumen::Loader::SymbolResolve query {};
	query.name                 = "7oxv3PPCumo";
	query.library              = "libkernel";
	query.library_version      = 1;
	query.module               = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	return query;
}

int32_t Reserve(void** address, uint64_t length, int32_t flags, uint64_t alignment)
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

bool TestBinding(Lumen::Loader::SymbolDatabase* symbols)
{
	if (!Check(symbols != nullptr && Lumen::Libs::Init(U"libkernel_1", symbols), "libkernel registration failed"))
	{
		return false;
	}
	const auto  exact  = Exact();
	const auto* symbol = symbols->Find(exact);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Memory::KernelReserveVirtualRange) &&
	               symbol->dbg_name == U"Memory::KernelReserveVirtualRange",
	           "reserve binding identity or target differs"))
	{
		return false;
	}
	g_reserve            = reinterpret_cast<ReserveFunction>(symbol->vaddr);
	g_symbols            = symbols;
	const auto* metadata = symbols->HostCallScalarMetadataFor(symbol->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 4 && metadata->arguments[0] == Lumen::Loader::HostCallScalarType::Gp &&
	               metadata->arguments[1] == Lumen::Loader::HostCallScalarType::Gp &&
	               metadata->arguments[2] == Lumen::Loader::HostCallScalarType::Gp &&
	               metadata->arguments[3] == Lumen::Loader::HostCallScalarType::Gp &&
	               metadata->result == Lumen::Loader::HostCallScalarType::Gp && metadata->invoker != nullptr,
	           "reserve scalar bridge metadata differs"))
	{
		return false;
	}

	for (int version: {0, 2})
	{
		auto wrong            = exact;
		wrong.library_version = version;
		if (!Check(symbols->Find(wrong) == nullptr, "wrong library version resolved"))
		{
			return false;
		}
		wrong                      = exact;
		wrong.module_version_major = version;
		if (!Check(symbols->Find(wrong) == nullptr, "wrong module major resolved"))
		{
			return false;
		}
		wrong                      = exact;
		wrong.module_version_minor = version;
		if (!Check(symbols->Find(wrong) == nullptr, "wrong module minor resolved"))
		{
			return false;
		}
	}
	for (const char* name: {"libSceKernel", "libkernelForKernel", "unrelated"})
	{
		auto wrong    = exact;
		wrong.library = name;
		if (!Check(symbols->Find(wrong) == nullptr, "wrong library namespace resolved"))
		{
			return false;
		}
		wrong        = exact;
		wrong.module = name;
		if (!Check(symbols->Find(wrong) == nullptr, "wrong module namespace resolved"))
		{
			return false;
		}
	}
	auto wrong_type = exact;
	wrong_type.type = Lumen::Loader::SymbolType::Object;
	return Check(symbols->Find(wrong_type) == nullptr && symbols->FindByStdName(U"7oxv3PPCumo") == nullptr &&
	                 symbols->FindByStdName(U"sceKernelReserveVirtualRange") == nullptr,
	             "reserve gained a type or standard-name fallback");
}

class GuestPages final
{
public:
	GuestPages(): address(Vm::Alloc(0, 2 * UNIT, Vm::Mode::ReadWrite))
	{
		if (address != 0)
		{
			std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), 0xa7, 2 * UNIT);
		}
	}

	~GuestPages()
	{
		if (address != 0) (void)Vm::Free(address);
	}

	GuestPages(const GuestPages&)            = delete;
	GuestPages& operator=(const GuestPages&) = delete;

	uint8_t* Data() const { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)); }

	void Set(size_t offset, uint64_t value) const { std::memcpy(Data() + offset, &value, sizeof(value)); }

	uint64_t Get(size_t offset) const
	{
		uint64_t value = 0;
		std::memcpy(&value, Data() + offset, sizeof(value));
		return value;
	}

	void** Record(size_t offset) const { return reinterpret_cast<void**>(Data() + offset); }

	uint64_t address = 0;
};

bool OutsideUnchanged(const GuestPages& pages, size_t offset, const std::array<uint8_t, 2 * UNIT>& before)
{
	for (size_t index = 0; index < before.size(); index++)
	{
		if ((index < offset || index >= offset + sizeof(uint64_t)) && pages.Data()[index] != before[index])
		{
			return false;
		}
	}
	return true;
}

bool TestScalarAndPointerErrors(GuestPages& pages)
{
	constexpr size_t offset = 37;
	pages.Set(offset, 0x1122334455667788ull);
	const uint64_t generation = Vm::GetMutationGeneration();
	struct Invalid
	{
		uint64_t requested;
		uint64_t length;
		int32_t  flags;
		uint64_t alignment;
	};
	for (const auto& sample: std::array<Invalid, 9> {{
	         {0, 0, 0, UNIT},
	         {0, UNIT + 1, 0, UNIT},
	         {0, UNIT, 0, 0},
	         {0, UNIT, 0, 3},
	         {0, UNIT, 0, UNIT / 2},
	         {0, UNIT, 0, UNIT * 3},
	         {0, UNIT, 1, UNIT},
	         {0, (std::numeric_limits<uint64_t>::max)() & ~(UNIT - 1), 0, UNIT},
	         {(std::numeric_limits<uint64_t>::max)() - UNIT + 1, UNIT, 0, UNIT},
	     }})
	{
		pages.Set(offset, sample.requested);
		const uint64_t before = pages.Get(offset);
		const int32_t  result = Reserve(pages.Record(offset), sample.length, sample.flags, sample.alignment);
		if (!Check(result == Kernel::KERNEL_ERROR_EINVAL && pages.Get(offset) == before, "invalid scalar changed output or error"))
		{
			return false;
		}
	}
	if (!Check(Vm::GetMutationGeneration() == generation, "invalid scalar changed VM generation"))
	{
		return false;
	}

	for (uint64_t address: {UINT64_C(0), UINT64_C(1), (std::numeric_limits<uint64_t>::max)() - 3})
	{
		if (!Check(Reserve(reinterpret_cast<void**>(address), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT,
		           "invalid guest record was accepted"))
		{
			return false;
		}
	}
	const uint64_t freed_record = Vm::Alloc(0, UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed_record != 0 && Vm::Free(freed_record), "freed-record fixture setup failed") ||
	    !Check(Reserve(reinterpret_cast<void**>(freed_record), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT,
	           "freed guest record was accepted"))
	{
		return false;
	}
	return true;
}

bool TestProtectedRecords(GuestPages& pages)
{
	constexpr size_t local = 64;
	pages.Set(local, 0);
	if (!Check(Vm::Protect(pages.address, UNIT, Vm::Mode::Read), "read-only setup failed"))
	{
		return false;
	}
	const bool read_only = Reserve(pages.Record(local), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT;
	if (!Check(Vm::Protect(pages.address, UNIT, Vm::Mode::ReadWrite), "read-only restoration failed") ||
	    !Check(read_only && pages.Get(local) == 0, "read-only record changed or succeeded"))
	{
		return false;
	}

	pages.Set(local, 0);
	if (!Check(Vm::Protect(pages.address, UNIT, Vm::Mode::NoAccess), "no-access setup failed"))
	{
		return false;
	}
	const bool no_access = Reserve(pages.Record(local), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT;
	if (!Check(Vm::Protect(pages.address, UNIT, Vm::Mode::ReadWrite), "no-access restoration failed") ||
	    !Check(no_access && pages.Get(local) == 0, "no-access record changed or succeeded"))
	{
		return false;
	}

	constexpr size_t crossing = UNIT - 4;
	pages.Set(crossing, 0);
	if (!Check(Vm::Protect(pages.address + UNIT, UNIT, Vm::Mode::NoAccess), "cross-boundary setup failed"))
	{
		return false;
	}
	const bool cross = Reserve(pages.Record(crossing), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT;
	if (!Check(Vm::Protect(pages.address + UNIT, UNIT, Vm::Mode::ReadWrite), "cross-boundary restoration failed") ||
	    !Check(cross && pages.Get(crossing) == 0, "cross-boundary record changed or succeeded"))
	{
		return false;
	}
	return true;
}

bool ReserveAndReset(GuestPages& pages, size_t offset, uint64_t requested)
{
	std::array<uint8_t, 2 * UNIT> before {};
	std::memcpy(before.data(), pages.Data(), before.size());
	pages.Set(offset, requested);
	std::memcpy(before.data() + offset, pages.Data() + offset, sizeof(uint64_t));
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == 0, "valid reservation failed"))
	{
		return false;
	}
	const uint64_t    base = pages.Get(offset);
	Vm::ManagedRegion region {};
	if (!Check(base != 0 && base % UNIT == 0 && (requested == 0 || base == requested) &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved &&
	               region.begin == base && region.end == base + UNIT && !Vm::IsManagedAddress(base) &&
	               OutsideUnchanged(pages, offset, before),
	           "success output, preferred address, canary, or reserved state differs"))
	{
		return false;
	}
	if (!Check(Memory::ResetForNextSession(), "reservation reset failed") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Unmanaged, "reset retained reservation"))
	{
		return false;
	}
	return true;
}

bool TestSuccessCollisionAndReset(GuestPages& pages)
{
	if (!ReserveAndReset(pages, 39, 0)) return false;

	const uint64_t candidate = Vm::AllocAligned(0, UNIT, Vm::Mode::ReadWrite, UNIT);
	if (!Check(candidate != 0 && Vm::Free(candidate), "exact candidate setup failed"))
	{
		return false;
	}
	if (!ReserveAndReset(pages, UNIT - sizeof(uint64_t), candidate))
	{
		return false;
	}

	constexpr size_t offset = 128;
	pages.Set(offset, pages.address);
	pages.Data()[0]       = 0x5c;
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == 0 && pages.Get(offset) != 0 &&
	               pages.Get(offset) != pages.address && pages.Data()[0] == 0x5c,
	           "preferred-address collision did not relocate or clobbered the existing mapping"))
	{
		return false;
	}
	const uint64_t relocated = pages.Get(offset);
	Vm::ManagedRegion relocated_region {};
	return Check(Vm::QueryManagedRegion(relocated, &relocated_region) == Vm::ManagedRegionState::Reserved &&
	                 relocated_region.begin == relocated && relocated_region.end == relocated + UNIT,
	             "relocated reservation was not registered") &&
	       Check(Memory::ResetForNextSession(), "preferred-address reservation reset failed") &&
	       Check(Vm::QueryManagedRegion(relocated, &relocated_region) == Vm::ManagedRegionState::Unmanaged,
	             "preferred-address reset retained reservation");
}

bool TestDirectMappingValidation(GuestPages& pages)
{
	constexpr size_t offset = 192;
	int64_t          physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 0, &physical) == 0,
	           "validation physical allocation failed"))
	{
		return false;
	}

	const uint64_t generation = Vm::GetMutationGeneration();
	if (!Check(Memory::KernelMapDirectMemory(nullptr, UNIT, 3, 0, physical, UNIT) == Kernel::KERNEL_ERROR_EINVAL,
	           "null direct output record was accepted") ||
	    !Check(Memory::KernelMapDirectMemory(reinterpret_cast<void**>(UINT64_C(1)), UNIT, 3, 0, physical, UNIT) ==
	               Kernel::KERNEL_ERROR_EINVAL,
	           "unreadable direct output record was accepted") ||
	    !Check(Vm::GetMutationGeneration() == generation, "invalid direct output record changed VM generation"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		return false;
	}

	struct Invalid
	{
		uint64_t requested;
		size_t   length;
		int      protection;
		int      flags;
		int64_t  direct_start;
		size_t   alignment;
	};
	const auto maximum_aligned = (std::numeric_limits<uint64_t>::max)() & ~(UNIT - 1);
	const auto maximum_signed_aligned =
	    static_cast<int64_t>(static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) & ~(UNIT - 1));
	const std::array<Invalid, 17> samples {{
	    {0, 0, 3, 0, physical, UNIT},
	    {0, UNIT + 1, 3, 0, physical, UNIT},
	    {0, UNIT, 8, 0, physical, UNIT},
	    {0, UNIT, 3, 1, physical, UNIT},
	    {0, UNIT, 3, 0, -1, UNIT},
	    {0, UNIT, 3, 0, physical + 1, UNIT},
	    {0, static_cast<size_t>(maximum_aligned), 3, 0, maximum_signed_aligned, UNIT},
	    {0, UNIT, 3, 0, physical, 0},
	    {0, UNIT, 3, 0, physical, 3},
	    {0, UNIT, 3, 0, physical, UNIT / 2},
	    {0, UNIT, 3, 0, physical, UNIT * 3},
	    {0, UNIT, 3, 0x10, physical, 0},
	    {UNIT + 1, UNIT, 3, 0x10, physical, 0},
	    {UNIT, UNIT, 3, 0x10, physical, 3},
	    {UNIT, UNIT, 3, 0x10, physical, UNIT / 2},
	    {UNIT, UNIT, 3, 0x10, physical, 2 * UNIT},
	    {(std::numeric_limits<uint64_t>::max)() - UNIT + 1, UNIT, 3, 0x10, physical, 0},
	}};
	for (const auto& sample: samples)
	{
		pages.Set(offset, sample.requested);
		std::array<uint8_t, 2 * UNIT> before {};
		std::memcpy(before.data(), pages.Data(), before.size());
		const int result = Memory::KernelMapDirectMemory(pages.Record(offset), sample.length, sample.protection, sample.flags,
		                                                 sample.direct_start, sample.alignment);
		if (!Check(result == Kernel::KERNEL_ERROR_EINVAL && pages.Get(offset) == sample.requested &&
		               std::memcmp(before.data(), pages.Data(), before.size()) == 0 && Vm::GetMutationGeneration() == generation,
		           "invalid direct geometry changed output, canary, generation, or error"))
		{
			(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
			(void)Memory::ResetForNextSession();
			return false;
		}
	}

	pages.Set(offset, 0);
	std::array<uint8_t, 2 * UNIT> before_owner_failure {};
	std::memcpy(before_owner_failure.data(), pages.Data(), before_owner_failure.size());
	if (!Check(Memory::KernelMapDirectMemory(pages.Record(offset), UNIT, 3, 0, physical + static_cast<int64_t>(UNIT), UNIT) ==
	               Kernel::KERNEL_ERROR_EBUSY &&
	               pages.Get(offset) == 0 && std::memcmp(before_owner_failure.data(), pages.Data(), before_owner_failure.size()) == 0 &&
	               Vm::GetMutationGeneration() == generation,
	           "missing direct owner mutated Core state or guest output") ||
	    !Check(Memory::KernelReleaseDirectMemory(physical, UNIT) == 0, "validation physical release failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	return true;
}

bool TestReservedDirectMapping(GuestPages& pages)
{
	constexpr size_t offset = 256;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 2 * UNIT, 0, UNIT) == 0, "parent reservation for direct mapping failed"))
	{
		return false;
	}
	const uint64_t base     = pages.Get(offset);
	int64_t        physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 0, &physical) == 0,
	           "direct physical allocation failed"))
	{
		return false;
	}

	void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical, 0) == 0 && reinterpret_cast<uint64_t>(mapped) == base,
	           "fixed direct activation with no additional alignment failed"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}
	auto* bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base));
	bytes[0]    = 0x6d;
	Vm::ManagedRegion region {};
	void*             query_start = nullptr;
	void*             query_end   = nullptr;
	int               query_prot  = 0;
	Memory::SetGpuOwnerLifecycleProbeForTest(true);
	const int         protect_result = Memory::KernelMprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), UNIT, 0x11);
	const int         repeat_protect_result = Memory::KernelMprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), UNIT, 0x11);
	Vm::ManagedRegion protected_region {};
	const bool        core_protected =
	    Vm::QueryManagedRegion(base, &protected_region) == Vm::ManagedRegionState::Visible && protected_region.mode == Vm::Mode::Read;
	const int query_result =
	    Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), &query_start, &query_end, &query_prot);
	const int  unmap_result  = Memory::KernelMunmap(base, UNIT);
	const auto gpu_lifecycle = Memory::GetGpuOwnerLifecycleStateForTest();
	Memory::SetGpuOwnerLifecycleProbeForTest(false);
	if (!Check(protect_result == 0 && repeat_protect_result == 0 && core_protected && query_result == 0 && query_prot == 0x11 &&
	               reinterpret_cast<uint64_t>(query_start) == base,
	           "whole-range mprotect did not update the direct owner query") ||
	    !Check(unmap_result == 0 && gpu_lifecycle.allocations == 1 && gpu_lifecycle.waits == 1 && gpu_lifecycle.frees == 1 &&
	               gpu_lifecycle.last_allocation_address == base && gpu_lifecycle.last_allocation_size == UNIT &&
	               gpu_lifecycle.last_free_address == base && gpu_lifecycle.last_free_size == UNIT,
	           "whole-range mprotect did not retain direct GPU cleanup") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved && !Vm::IsManagedAddress(base),
	           "decommitted direct child did not restore reservation"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}

	mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical, UNIT) == 0 && bytes[0] == 0x6d,
	           "direct child reactivation lost its physical backing") ||
	    !Check(Memory::KernelReleaseDirectMemory(physical, UNIT) == 0, "two-phase direct release failed") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved,
	           "direct release removed the parent reservation") ||
	    !Check(Memory::ResetForNextSession(), "parent cleanup after direct release failed") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Unmanaged, "parent survived final reset"))
	{
		return false;
	}
	return true;
}

bool TestPhysicalBackingHandoff(GuestPages& pages)
{
	using Lumen::GuestCpu::MemoryAccessIntent;
	using Lumen::GuestCpu::NeedsHandoff;
	constexpr size_t offset = 320;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 3 * UNIT, 0, UNIT) == 0, "handoff parent reserve failed")) return false;
	const uint64_t base = pages.Get(offset);
	int64_t physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, INT64_MAX, 2 * UNIT, UNIT, 0, &physical) == 0,
	           "handoff physical allocation failed")) return false;
	void* mapped = reinterpret_cast<void*>(base);
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0,
	           "handoff offset mapping failed")) return false;
	Vm::GuestBackingSpan original;
	if (!Check(Vm::QueryGuestBackingSpan(base, UNIT, &original) && original.identity != 0 && original.offset == UNIT,
	           "handoff physical identity/offset missing")) return false;
	const auto mark = [&](uint64_t address, uint32_t intents) {
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		if (token == 0) return false;
		bool ok = false;
		{
			Vm::GuestMemoryMutationOwnerScope owner(token);
			ok = owner.IsActive() && Vm::MarkGuestBackingHandoffRange(address, UNIT, intents);
		}
		return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && ok;
	};
	if (!Check(!NeedsHandoff(base, 3 * UNIT, MemoryAccessIntent::Read) && !NeedsHandoff(base, UNIT, MemoryAccessIntent::Write) &&
	               !mark(base, 0) && !mark(base, 4) && !NeedsHandoff(base, UNIT, MemoryAccessIntent::Write),
	           "handoff split fixture requires fresh guards; invalid intent must not enroll them")) return false;
	std::memset(mapped, 0x39, UNIT);
	// Private publication retains exact physical identity independently of
	// guest VA rights. Use the real FEX exclusive owner and the real Core
	// local gate; no fixture-owned fd or substitute owner participates.
	const auto transfer = [&](uint64_t address, uint8_t value) {
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		if (token == 0) return false;
		bool ok = false;
		{
			Vm::GuestMemoryMutationOwnerScope owner(token);
			uint8_t observed = 0;
			Vm::GuestDataLease denied;
			ok = owner.IsActive() && !Vm::TryAcquireGuestDataLease(address + 13, 1, Vm::Mode::Write, &denied, false) &&
			    Vm::CopyGuestBackingDataForGpu(address + 13, &value, 1, true) &&
			    Vm::CopyGuestBackingDataForGpu(address + 13, &observed, 1, false) && observed == value &&
			    !Vm::CopyGuestBackingDataForGpu(address + UNIT - 1, &observed, 2, false);
			ok = Vm::MarkGuestBackingHandoffRange(address, UNIT, static_cast<uint32_t>(MemoryAccessIntent::Write)) && ok;
		}
		return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && ok;
	};
	Vm::ManagedRegion region;
	if (!Check(Memory::KernelApplyPosixProtection(base, UNIT, 1) == 0 && transfer(base, 0x62) &&
	               static_cast<const uint8_t*>(mapped)[13] == 0x62 &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Visible && region.mode == Vm::Mode::Read,
	           "handoff private copy did not preserve read-only guest rights")) return false;
	if (!Check(Memory::KernelApplyPosixProtection(base, UNIT, 0) == 0 && transfer(base, 0xa7) &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Visible && region.mode == Vm::Mode::NoAccess &&
	               !NeedsHandoff(base, UNIT, MemoryAccessIntent::Read) && NeedsHandoff(base, UNIT, MemoryAccessIntent::Write) &&
	               NeedsHandoff(base, UNIT, MemoryAccessIntent::ReadWrite),
	           "handoff private copy did not preserve NoAccess guest rights")) return false;
	if (!Check(Memory::KernelMunmap(base, UNIT) == 0 &&
	               Vm::QueryGuestBackingPresence(original.identity, original.offset, UNIT) == Vm::GuestBackingPresence::Retired,
	           "handoff unmapped alias incorrectly remains live")) return false;
	mapped = reinterpret_cast<void*>(base + UNIT);
	Vm::GuestBackingSpan remapped;
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0 &&
	               Vm::QueryGuestBackingSpan(base + UNIT, UNIT, &remapped) && remapped.identity == original.identity &&
	               remapped.offset == original.offset && static_cast<const uint8_t*>(mapped)[13] == 0xa7 &&
	               static_cast<const uint8_t*>(mapped)[12] == 0x39 && static_cast<const uint8_t*>(mapped)[14] == 0x39 &&
	               !NeedsHandoff(base + UNIT + 13, 1, MemoryAccessIntent::Read) &&
	               NeedsHandoff(base + UNIT + 13, 1, MemoryAccessIntent::Write),
	           "handoff remap lost identity, exact bytes, canaries, or inherited Write-only watch")) return false;
	if (!Check(mark(base + UNIT, static_cast<uint32_t>(MemoryAccessIntent::Read)) &&
	               NeedsHandoff(base + UNIT, UNIT, MemoryAccessIntent::Read) &&
	               NeedsHandoff(base + UNIT, UNIT, MemoryAccessIntent::Write) &&
	               Memory::KernelMunmap(base + UNIT, UNIT) == 0,
	           "handoff Read upgrade did not retain both watch intents")) return false;
	mapped = reinterpret_cast<void*>(base + 2 * UNIT);
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0 &&
	               Vm::QueryGuestBackingSpan(base + 2 * UNIT, UNIT, &remapped) && remapped.identity == original.identity &&
	               remapped.offset == original.offset && static_cast<const uint8_t*>(mapped)[13] == 0xa7 &&
	               NeedsHandoff(base + 2 * UNIT, UNIT, MemoryAccessIntent::Read) &&
	               NeedsHandoff(base + 2 * UNIT, UNIT, MemoryAccessIntent::Write),
	           "handoff upgraded physical owner did not carry both intents into a fresh mapping")) return false;
	const bool released = Memory::KernelReleaseDirectMemory(physical, 2 * UNIT) == 0 && Memory::ResetForNextSession();
	std::printf("KernelVirtualRangeTest: physical-handoff=%s rights=R/NoAccess alias-remap=2 exact-publication=1"
	            " split-write-inheritance=1 read-upgrade-inheritance=1 invalid-intent-rejected=1\n", released ? "ok" : "fail");
	return Check(released && Vm::QueryGuestBackingPresence(original.identity, original.offset, UNIT) == Vm::GuestBackingPresence::Retired,
	    "handoff physical lifetime cleanup failed");
}

bool TestOwnedPhysicalBackingHandoff(GuestPages& pages)
{
	using Lumen::GuestCpu::MemoryAccessIntent;
	using Lumen::GuestCpu::NeedsHandoff;
	constexpr size_t offset = 352;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 3 * UNIT, 0, UNIT) == 0, "owned handoff reserve failed")) return false;
	const uint64_t base = pages.Get(offset);
	int64_t physical = -1;
	void* mapped = reinterpret_cast<void*>(base);
	if (!Check(Memory::KernelAllocateDirectMemory(0, INT64_MAX, 2 * UNIT, UNIT, 0, &physical) == 0 &&
	               Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0,
	           "owned handoff physical offset map failed")) return false;
	const auto exclusive = [](auto operation) {
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		if (token == 0) return false;
		bool ok = false;
		{
			Vm::GuestMemoryMutationOwnerScope epoch(token);
			ok = epoch.IsActive() && operation();
		}
		return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && ok;
	};
	uint64_t input = 0, peer = 0;
	if (!Check(!NeedsHandoff(base, 3 * UNIT) && exclusive([&] {
		    input = Vm::CreateGuestBackingHandoffOwner();
		    peer = Vm::CreateGuestBackingHandoffOwner();
		    return input != 0 && peer != 0 &&
		        !Vm::MarkGuestBackingHandoffRangeOwned(input, base + 2 * UNIT, UNIT, 3) &&
		        Vm::MarkGuestBackingHandoffRangeOwned(input, base, UNIT, 2) &&
		        Vm::MarkGuestBackingHandoffRangeOwned(peer, base + 8, 1, 1);
	    }) && NeedsHandoff(base, UNIT, MemoryAccessIntent::Read) &&
	        NeedsHandoff(base, UNIT, MemoryAccessIntent::Write),
	    "owned handoff enrollment, peer or unknown source check failed")) return false;
	if (!Check(exclusive([&] { return Vm::ReleaseGuestBackingHandoffOwner(peer); }) &&
	               !NeedsHandoff(base, UNIT, MemoryAccessIntent::Read) && NeedsHandoff(base, UNIT, MemoryAccessIntent::Write),
	           "owned read peer retirement lost the live write guard")) return false;
	Vm::GuestBackingSpan original, remapped;
	if (!Check(Vm::QueryGuestBackingSpan(base, UNIT, &original) && Memory::KernelMunmap(base, UNIT) == 0,
	           "owned handoff initial unmap failed")) return false;
	mapped = reinterpret_cast<void*>(base + UNIT);
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0 &&
	               Vm::QueryGuestBackingSpan(base + UNIT, UNIT, &remapped) && original.identity == remapped.identity &&
	               original.offset == remapped.offset && NeedsHandoff(base + UNIT, UNIT, MemoryAccessIntent::Write) &&
	               !NeedsHandoff(base + UNIT, UNIT, MemoryAccessIntent::Read),
	           "owned physical intent was lost on remap")) return false;
	if (!Check(exclusive([&] { return Vm::MarkGuestBackingHandoffRangeOwned(input, base + UNIT, UNIT, 1); }) &&
	               NeedsHandoff(base + UNIT, UNIT, MemoryAccessIntent::Read) &&
	               exclusive([&] { return Vm::ReleaseGuestBackingHandoffOwner(input) &&
	                   !Vm::ReleaseGuestBackingHandoffOwner(input); }) && !NeedsHandoff(base, 3 * UNIT),
	           "owned retirement did not remove all historical aliases or reject double release")) return false;
	if (!Check(Memory::KernelMunmap(base + UNIT, UNIT) == 0, "owned handoff second unmap failed")) return false;
	mapped = reinterpret_cast<void*>(base + 2 * UNIT);
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical + UNIT, UNIT) == 0 &&
	               !NeedsHandoff(base, 3 * UNIT) && Memory::KernelReleaseDirectMemory(physical, 2 * UNIT) == 0 &&
	               Memory::ResetForNextSession(),
	           "retired resource re-enrolled guards on a later physical remap")) return false;
	std::printf("KernelVirtualRangeTest: owned-handoff=ok peer-retirement=1 remap=2 read-upgrade=1 historical-guards-retired=1 unknown-source=denied\n");
	return true;
}

bool TestReservedFlexibleMapping(GuestPages& pages)
{
	constexpr size_t offset = 384;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 3 * UNIT, 0, UNIT) == 0, "parent reservation for flexible mapping failed"))
	{
		return false;
	}
	const uint64_t base   = pages.Get(offset);
	void*          mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapFlexibleMemory(&mapped, 2 * UNIT, 3, 0) == 0 && reinterpret_cast<uint64_t>(mapped) == base,
	           "flexible activation inside reservation failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	auto* bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base));
	bytes[0]    = 0x4e;
	Vm::ManagedRegion region {};
	void*             query_start = nullptr;
	void*             query_end   = nullptr;
	int               query_prot  = 0;
	Memory::SetGpuOwnerLifecycleProbeForTest(true);
	const int         protect_result = Memory::KernelMprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(base + UNIT)), UNIT, 0x11);
	const int repeat_protect_result =
	    Memory::KernelMprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(base + UNIT)), UNIT, 0x11);
	Vm::ManagedRegion protected_region {};
	const bool        core_protected = Vm::QueryManagedRegion(base + UNIT, &protected_region) == Vm::ManagedRegionState::Visible &&
	                            protected_region.begin == base + UNIT && protected_region.mode == Vm::Mode::Read;
	const int query_result = Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base + UNIT)), &query_start,
	                                                             &query_end, &query_prot);
	const int unmap_result = Memory::KernelMunmap(base, 2 * UNIT);
	const auto gpu_lifecycle = Memory::GetGpuOwnerLifecycleStateForTest();
	Memory::SetGpuOwnerLifecycleProbeForTest(false);
	if (!Check(protect_result == 0 && repeat_protect_result == 0 && core_protected && query_result == 0 && query_prot == 3 &&
	               reinterpret_cast<uint64_t>(query_start) == base,
	           "partial mprotect changed the coarse flexible owner query") ||
	    !Check(unmap_result == 0 && gpu_lifecycle.allocations == 1 && gpu_lifecycle.waits == 1 && gpu_lifecycle.frees == 1 &&
	               gpu_lifecycle.last_allocation_address == base && gpu_lifecycle.last_allocation_size == 2 * UNIT &&
	               gpu_lifecycle.last_free_address == base && gpu_lifecycle.last_free_size == 2 * UNIT,
	           "partial mprotect did not conservatively retain flexible GPU cleanup") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved,
	           "flexible child decommit did not restore reservation"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapFlexibleMemory(&mapped, UNIT, 3, 0) == 0 && bytes[0] == 0,
	           "flexible child reactivation was not zero-filled") ||
	    !Check(Memory::ResetForNextSession(), "flexible child and parent reset failed") ||
	    !Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Unmanaged, "flexible parent survived reset"))
	{
		return false;
	}
	return true;
}

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK) && defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
bool TestOwnerIdentityDuringCoreRelease()
{
	// A native borrower still owns its VM reader when Free starts waiting.
	// Resolving its physical identity must remain possible until that reader
	// releases; retaining the owner mutex across Free would deadlock this cycle.
	for (unsigned operation = 0; operation != 5; ++operation)
	{
		int64_t physical = -1;
		void* mapped = nullptr;
		if (operation != 2 && operation != 4)
		{
			if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(),
			                                              UNIT, UNIT, 0, &physical) == 0,
			           "owner-release physical allocation failed") ||
			    !Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0, physical, UNIT) == 0,
			           "owner-release physical map failed")) return false;
		} else if (!Check(Memory::KernelMapFlexibleMemory(&mapped, UNIT, 3, 0) == 0,
		                 "owner-release flexible map failed")) return false;
		const auto address = reinterpret_cast<uint64_t>(mapped);
		std::atomic<int> borrower {0}, query {0}, release {0};
		std::thread reader([&] {
			Vm::GuestDataLease input;
			if (!Vm::TryAcquireGuestDataLease(address, 1, Vm::Mode::Read, &input))
			{
				borrower.store(-1, std::memory_order_release);
				return;
			}
			borrower.store(1, std::memory_order_release);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (Vm::GetVirtualRangeTransactionStateForTest().waiting_mutations == 0)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					query.store(-1, std::memory_order_release);
					return;
				}
				std::this_thread::yield();
			}
			Vm::GuestBackingSpan span;
			const bool resolved = Vm::QueryGuestBackingSpan(address, 1, &span) &&
			                      span.address == address && span.size == 1 && span.identity != 0;
			query.store(resolved ? 1 : -1, std::memory_order_release);
		});
		if (!WaitForResult(borrower, 0) || borrower.load(std::memory_order_acquire) != 1)
		{
			reader.join();
			(void)Memory::ResetForNextSession();
			return false;
		}
		std::thread mutator([&] {
			const int result = operation >= 3 ? (Memory::ResetForNextSession() ? 0 : -1)
			                   : operation == 0 ? Memory::KernelReleaseDirectMemory(physical, UNIT)
			                                    : Memory::KernelMunmap(address, UNIT);
			release.store(result == 0 ? 1 : -1, std::memory_order_release);
		});
		if (!WaitForResult(query, 0))
		{
			std::fprintf(stderr, "KernelVirtualRangeTest: owner identity deadlocked during Core release operation=%u\n", operation);
			std::_Exit(1);
		}
		reader.join();
		mutator.join();
		if (!Check(query.load(std::memory_order_acquire) == 1 && release.load(std::memory_order_acquire) == 1,
		           "owner identity or Core release failed while a foreign borrower was active")) return false;
		Vm::GuestBackingSpan retired;
		if (!Check(!Vm::QueryGuestBackingSpan(address, 1, &retired),
		           "retired mapping retained an authoritative backing identity")) return false;
		if (operation == 1 && !Check(Memory::KernelReleaseDirectMemory(physical, UNIT) == 0,
		                            "owner-release unmapped physical cleanup failed")) return false;
	}
	return Memory::ResetForNextSession();
}

bool TestMappingFailureTransactions(GuestPages& pages)
{
	constexpr size_t offset = 768;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 2 * UNIT, 0, UNIT) == 0, "mapping-fault parent reservation failed"))
	{
		return false;
	}
	const uint64_t base     = pages.Get(offset);
	int64_t        physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 0, &physical) == 0,
	           "mapping-fault physical allocation failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	const uint64_t before_activate = Vm::GetMutationGeneration();
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::ActivateHost);
	void*             mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	Vm::ManagedRegion region {};
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical, UNIT) == Kernel::KERNEL_ERROR_EBUSY && mapped == nullptr &&
	               Vm::GetMutationGeneration() == before_activate &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved,
	           "activate-host failure changed owner or Core state"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}
	mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	if (!Check(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical, UNIT) == 0, "mapping-fault activation retry failed"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}

	const uint64_t before_decommit = Vm::GetMutationGeneration();
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::FexExclusiveAdmission);
	if (!Check(Memory::KernelMunmap(base, UNIT) == Kernel::KERNEL_ERROR_EBUSY && Vm::GetMutationGeneration() == before_decommit &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Visible,
	           "exclusive denial lost the direct owner"))
	{
		(void)Memory::KernelReleaseDirectMemory(physical, UNIT);
		(void)Memory::ResetForNextSession();
		return false;
	}
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::DecommitHost);
	if (!Check(Memory::KernelMunmap(base, UNIT) == Kernel::KERNEL_ERROR_EBUSY && Vm::GetMutationGeneration() == before_decommit &&
	               Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Visible,
	           "decommit-host failure lost the direct owner") ||
	    !Check(Memory::KernelMunmap(base, UNIT) == 0, "direct decommit retry failed") ||
	    !Check(Memory::KernelReleaseDirectMemory(physical, UNIT) == 0, "unmapped physical owner release failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::ReleaseHost);
	if (!Check(!Memory::ResetForNextSession(), "release-host failure did not block reset"))
	{
		return false;
	}
	const auto state = Memory::GetVirtualRangeStateForTest();
	if (!Check(state.live == 1 && state.pending_cleanup == 1 && Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Reserved,
	           "failed parent release lost retry state") ||
	    !Check(Memory::ResetForNextSession(), "parent release retry failed"))
	{
		return false;
	}
	return Check(Vm::QueryManagedRegion(base, &region) == Vm::ManagedRegionState::Unmanaged, "parent release retry retained Core state");
}

bool TestLifecycleCallbackOrdering(GuestPages& pages)
{
	constexpr size_t offset = 896;
	pages.Set(offset, 0);
	if (!Check(Reserve(pages.Record(offset), 2 * UNIT, 0, UNIT) == 0, "lifecycle parent reservation failed"))
	{
		return false;
	}
	const uint64_t base     = pages.Get(offset);
	int64_t        physical = -1;
	if (!Check(Memory::KernelAllocateDirectMemory(0, (std::numeric_limits<int64_t>::max)(), UNIT, UNIT, 0, &physical) == 0,
	           "lifecycle physical allocation failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}

	if (!Check(Memory::RegisterCallbacks(AllocationCallback, FreeCallback), "lifecycle callback registration failed"))
	{
		(void)Memory::ResetForNextSession();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
		g_callback_probe.block_alloc   = true;
		g_callback_probe.block_free    = true;
		g_callback_probe.alloc_entered = false;
		g_callback_probe.free_entered  = false;
		g_callback_probe.alloc_calls   = 0;
		g_callback_probe.free_calls    = 0;
		g_callback_probe.alloc_address = 0;
		g_callback_probe.free_address  = 0;
		g_callback_probe.alloc_size    = 0;
		g_callback_probe.free_size     = 0;
	}
	const auto lifecycle_before_contention = Memory::GetMemoryLifecycleStateForTest();

	constexpr int    pending = (std::numeric_limits<int>::min)();
	std::atomic<int> initial_map_result {pending};
	std::atomic<int> contended_unmap_result {pending};
	std::atomic<int> committed_unmap_result {pending};
	std::atomic<int> contended_flexible_result {pending};
	std::thread      initial_map(
        [&]
        {
            void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
            initial_map_result.store(Memory::KernelMapDirectMemory(&mapped, UNIT, 3, 0x10, physical, UNIT), std::memory_order_release);
        });

	const bool        alloc_entered = WaitForCallback(true);
	Vm::ManagedRegion callback_region {};
	void*             query_start = nullptr;
	void*             query_end   = nullptr;
	int               query_prot  = 0;
	struct DirectQueryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};
	DirectQueryInfo direct_query {};
	size_t          flexible_available = 0;
	const bool      peer_queries_denied_during_map =
	    Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), &query_start, &query_end, &query_prot) ==
	        Kernel::KERNEL_ERROR_EBUSY &&
	    Memory::KernelDirectMemoryQuery(physical, 0, &direct_query, sizeof(direct_query)) == Kernel::KERNEL_ERROR_EBUSY &&
	    Memory::KernelAvailableFlexibleMemorySize(&flexible_available) == Kernel::KERNEL_ERROR_EBUSY;
	const bool callback_replacement_denied_during_map = !Memory::RegisterCallbacks(UnusedCallback, UnusedCallback);
	const bool map_visible_before_callback_return =
	    alloc_entered && Vm::QueryManagedRegion(base, &callback_region) == Vm::ManagedRegionState::Visible &&
	    peer_queries_denied_during_map && callback_replacement_denied_during_map && Memory::GetMemoryLifecycleStateForTest().active == 1 &&
	    Vm::GetVirtualRangeTransactionStateForTest().active == 0;
	std::thread contended_unmap;
	bool        unmap_denied_promptly = false;
	if (alloc_entered)
	{
		contended_unmap = std::thread([&] { contended_unmap_result.store(Memory::KernelMunmap(base, UNIT), std::memory_order_release); });
		unmap_denied_promptly = WaitForResult(contended_unmap_result, pending);
	}
	if (unmap_denied_promptly)
	{
		{
			std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
			g_callback_probe.block_alloc = false;
		}
		g_callback_probe.ready.notify_all();
	} else
	{
		ReleaseCallbackBlocks();
	}
	initial_map.join();
	if (contended_unmap.joinable()) contended_unmap.join();
	query_start                      = nullptr;
	query_end                        = nullptr;
	query_prot                       = 0;
	const bool owner_query_after_map = Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
	                                                                       &query_start, &query_end, &query_prot) == 0 &&
	                                   reinterpret_cast<uint64_t>(query_start) == base && query_prot == 3;

	const int  first_result     = initial_map_result.load(std::memory_order_acquire);
	const int  first_contention = contended_unmap_result.load(std::memory_order_acquire);
	const bool first_phase = alloc_entered && unmap_denied_promptly && first_result == 0 && first_contention == Kernel::KERNEL_ERROR_EBUSY;

	std::thread committed_unmap;
	if (first_phase)
	{
		{
			std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
			g_callback_probe.free_entered = false;
			g_callback_probe.block_free   = true;
		}
		committed_unmap = std::thread([&] { committed_unmap_result.store(Memory::KernelMunmap(base, UNIT), std::memory_order_release); });
	}
	const bool free_entered           = committed_unmap.joinable() && WaitForCallback(false);
	query_start                       = nullptr;
	query_end                         = nullptr;
	query_prot                        = 0;
	const auto core_state_during_free = Vm::QueryManagedRegion(base, &callback_region);
	const int  owner_query_during_free =
	    Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), &query_start, &query_end, &query_prot);
	const bool tombstone_visible_during_free =
	    free_entered && core_state_during_free == Vm::ManagedRegionState::Reserved && owner_query_during_free == Kernel::KERNEL_ERROR_EBUSY;
	std::thread contended_flexible;
	bool        flexible_denied_promptly = false;
	if (free_entered)
	{
		contended_flexible = std::thread(
		    [&]
		    {
			    void* mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
			    contended_flexible_result.store(Memory::KernelMapFlexibleMemory(&mapped, UNIT, 3, 0), std::memory_order_release);
		    });
		flexible_denied_promptly = WaitForResult(contended_flexible_result, pending);
	}
	ReleaseCallbackBlocks();
	if (committed_unmap.joinable()) committed_unmap.join();
	if (contended_flexible.joinable()) contended_flexible.join();

	const int first_unmap       = committed_unmap_result.load(std::memory_order_acquire);
	const int second_contention = contended_flexible_result.load(std::memory_order_acquire);
	query_start                 = nullptr;
	query_end                   = nullptr;
	query_prot                  = 0;
	const bool owner_hidden_after_unmap =
	    first_unmap == 0 && Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), &query_start,
	                                                            &query_end, &query_prot) == Kernel::KERNEL_ERROR_EACCES;
	void*     flexible_address = reinterpret_cast<void*>(static_cast<uintptr_t>(base));
	const int flexible_retry   = first_unmap == 0 && second_contention == Kernel::KERNEL_ERROR_EBUSY
	                                 ? Memory::KernelMapFlexibleMemory(&flexible_address, UNIT, 3, 0)
	                                 : pending;
	uint64_t  alloc_calls      = 0;
	uint64_t  free_calls       = 0;
	uintptr_t alloc_address    = 0;
	uintptr_t free_address     = 0;
	size_t    alloc_size       = 0;
	size_t    free_size        = 0;
	{
		std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
		alloc_calls   = g_callback_probe.alloc_calls;
		free_calls    = g_callback_probe.free_calls;
		alloc_address = g_callback_probe.alloc_address;
		free_address  = g_callback_probe.free_address;
		alloc_size    = g_callback_probe.alloc_size;
		free_size     = g_callback_probe.free_size;
	}

	bool mapping_live = first_result == 0;
	if (first_contention == 0 || first_unmap == 0) mapping_live = false;
	if (second_contention == 0 || flexible_retry == 0) mapping_live = true;
	bool cleanup_ok = !mapping_live || Memory::KernelMunmap(base, UNIT) == 0;
	if (physical >= 0)
	{
		cleanup_ok = Memory::KernelReleaseDirectMemory(physical, UNIT) == 0 && cleanup_ok;
	}
	cleanup_ok                 = Memory::ResetForNextSession() && cleanup_ok;
	const auto final_lifecycle = Memory::GetMemoryLifecycleStateForTest();

	return Check(first_phase, "contended unmap did not fail fast during alloc callback") &&
	       Check(map_visible_before_callback_return && owner_query_after_map, "map commit was not visible throughout its callback") &&
	       Check(first_result == 0 && free_entered, "initial map or free callback ordering failed") &&
	       Check(core_state_during_free == Vm::ManagedRegionState::Reserved, "Core decommit was not visible during free callback") &&
	       Check(owner_query_during_free == Kernel::KERNEL_ERROR_EBUSY && tombstone_visible_during_free,
	             "HLE owner query was not guarded during free callback") &&
	       Check(flexible_denied_promptly && second_contention == Kernel::KERNEL_ERROR_EBUSY,
	             "cross-owner remap did not fail fast during free callback") &&
	       Check(owner_hidden_after_unmap && flexible_retry == 0 && reinterpret_cast<uint64_t>(flexible_address) == base,
	             "physical-to-flexible retry failed after cleanup") &&
	       Check(alloc_calls == 2 && free_calls == 1 && alloc_address == base && free_address == base && alloc_size == UNIT &&
	                 free_size == UNIT,
	             "callback counts before cleanup differ") &&
	       Check(cleanup_ok && final_lifecycle.active == 0 && final_lifecycle.waiting == 0 &&
	                 final_lifecycle.try_denials == lifecycle_before_contention.try_denials + 7,
	             "lifecycle cleanup retained an owner or gate");
}

struct LifecycleFexRun
{
	GuestPages code, stack;
	bool hold_lease = false;
	bool lease_acquired = false;
	unsigned calls = 0;
	std::atomic<bool> entered {false};
	bool competing_writer = false;
	bool conflict_observed = false;
	std::atomic<bool> reserve_returned {false};
};

uint32_t QueryLifecycleFexCode(void*, uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) noexcept
{
	constexpr uint64_t guest_page = 4096;
	const uint64_t page = address & ~(guest_page - 1);
	Vm::CodeReadLease lease;
	if (!Vm::TryAcquireCodeReadLease(page, guest_page, &lease)) return 0;
	*base = page;
	*size = guest_page;
	*writable = 0;
	Vm::ReleaseCodeReadLease(&lease);
	return 1;
}

uint32_t LifecycleFexHostCall(void* opaque, uint64_t target, uint64_t* gprs, uint64_t* next) noexcept
{
	auto& fixture = *static_cast<LifecycleFexRun*>(opaque);
	if (target != reinterpret_cast<uint64_t>(g_reserve)) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	++fixture.calls;
	if (fixture.competing_writer)
	{
		fixture.entered.store(true, std::memory_order_release);
		// Observe the real provider's pending exclusive epoch while retaining
		// this HLE reader. Successful probes close without any mapping effect;
		// the competing writer cannot finish before Reserve donates the reader.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		do
		{
			uint32_t retryable = 0;
			const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2, &retryable);
			if (token != 0)
			{
				if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) != 1)
					return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
			}
			else if (retryable == 1) { fixture.conflict_observed = true; break; }
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < deadline);
		if (!fixture.conflict_observed) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	}
	{
		Vm::GuestDataLease lease;
		if (fixture.hold_lease)
		{
			fixture.lease_acquired = Vm::TryAcquireGuestDataLease(gprs[7], sizeof(uint64_t), Vm::Mode::Write, &lease);
			if (!fixture.lease_acquired) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
		}
		fixture.entered.store(true, std::memory_order_release);
		gprs[0] = static_cast<uint32_t>(Reserve(reinterpret_cast<void**>(gprs[7]), gprs[6], static_cast<int32_t>(gprs[2]), gprs[1]));
		fixture.reserve_returned.store(true, std::memory_order_release);
	}
	if (!Lumen::Loader::ReadGuestMemory(gprs[4], next, sizeof(*next))) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	gprs[4] += sizeof(*next);
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool TestQuiescentLifecycleReservation()
{
	// A real FEX root calls the scalar bridge while another thread owns the
	// HLE lifecycle inside an allocation callback. The callback then performs
	// an Exclusive Core mutation before releasing that lifecycle. Keeping the
	// peer HLE reader or acquiring lifecycle before readmission would deadlock.
	for (unsigned sample = 0; sample != 3; ++sample)
	{
		const bool free_output = sample == 1;
		const bool hold_lease = sample == 2;
		constexpr size_t record_offset = 37, owner_offset = 128;
		LifecycleFexRun fixture;
		GuestPages record, owner_record, mutation;
		if (!Check(fixture.code.address != 0 && fixture.stack.address != 0 && record.address != 0 &&
		               owner_record.address != 0 && mutation.address != 0,
		           "quiescent lifecycle fixture allocations failed")) return false;
		fixture.hold_lease = hold_lease;
		record.Set(record_offset, 0);
		owner_record.Set(owner_offset, 0);
		std::array<uint8_t, 2 * UNIT> record_before;
		std::memcpy(record_before.data(), record.Data(), record_before.size());
		// Original x86-64 leaf: movabs rax,registered Reserve; call rax; ret.
		std::array<uint8_t, 13> program {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0xc3};
		const uint64_t target = reinterpret_cast<uint64_t>(g_reserve);
		std::memcpy(program.data() + 2, &target, sizeof(target));
		std::memcpy(fixture.code.Data(), program.data(), program.size());
		if (!Check(Vm::Protect(fixture.code.address, 2 * UNIT, Vm::Mode::ExecuteRead) &&
		               LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1 &&
		               Memory::RegisterCallbacks(AllocationCallback, FreeCallback),
		           "quiescent lifecycle fixture publication failed")) return false;
		{
			std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
			g_callback_probe.block_alloc = true;
			g_callback_probe.block_free = false;
			g_callback_probe.alloc_entered = false;
			g_callback_probe.mutate_after_alloc = true;
			g_callback_probe.free_during_alloc = free_output;
			g_callback_probe.mutation_address = free_output ? record.address : mutation.address;
			g_callback_probe.mutation_size = 2 * UNIT;
			g_callback_probe.reentry_record = record.Record(record_offset);
			g_callback_probe.reentry_result = 0;
			g_callback_probe.mutation_ok = false;
		}
		const auto before = Memory::GetMemoryLifecycleStateForTest();
		std::atomic<int> map_result {(std::numeric_limits<int>::min)()};
		std::thread owner([&] {
			map_result.store(Memory::KernelMapFlexibleMemory(owner_record.Record(owner_offset), UNIT, 3, 0),
			                 std::memory_order_release);
		});
		const bool owner_entered = WaitForCallback(true);
		bool waiting_observed = false;
		std::thread release_owner([&] {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (Memory::GetMemoryLifecycleStateForTest().waiting != 0)
				{
					waiting_observed = true;
					break;
				}
				if (hold_lease && fixture.entered.load(std::memory_order_acquire)) break;
				std::this_thread::yield();
			}
			ReleaseCallbackBlocks();
		});
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = fixture.code.address;
		state.gprs[4] = fixture.stack.address + 2 * UNIT - sizeof(uint64_t);
		state.gprs[7] = reinterpret_cast<uint64_t>(record.Record(record_offset));
		state.gprs[6] = UNIT;
		state.gprs[2] = 0;
		state.gprs[1] = UNIT;
		const uint64_t root = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root, sizeof(root));
		const uint32_t run_result = owner_entered
		    ? LumenAndroidFexCoreRunGuestV1(&state, LifecycleFexHostCall, QueryLifecycleFexCode, &fixture)
		    : UINT32_MAX;
		release_owner.join();
		owner.join();
		bool mutation_ok;
		int reentry_result;
		{
			std::lock_guard<std::mutex> lock(g_callback_probe.mutex);
			mutation_ok = g_callback_probe.mutation_ok;
			reentry_result = g_callback_probe.reentry_result;
			g_callback_probe.mutate_after_alloc = false;
			g_callback_probe.reentry_record = nullptr;
		}
		const uint64_t output_address = record.address;
		if (free_output && mutation_ok) record.address = 0;
		const int expected = hold_lease ? Kernel::KERNEL_ERROR_ENOMEM : free_output ? Kernel::KERNEL_ERROR_EFAULT : 0;
		Vm::ManagedRegion region {};
		const bool record_correct = free_output
		    ? Vm::QueryManagedRegion(output_address, &region) == Vm::ManagedRegionState::Unmanaged
		    : OutsideUnchanged(record, record_offset, record_before) &&
		      (hold_lease ? record.Get(record_offset) == 0
		                  : record.Get(record_offset) != 0 &&
		                    Vm::QueryManagedRegion(record.Get(record_offset), &region) == Vm::ManagedRegionState::Reserved);
		const auto after = Memory::GetMemoryLifecycleStateForTest();
		const bool cleanup = Memory::ResetForNextSession();
		const auto final_state = Memory::GetMemoryLifecycleStateForTest();
		const bool passed = owner_entered && map_result.load(std::memory_order_acquire) == 0 && mutation_ok &&
		    reentry_result == Kernel::KERNEL_ERROR_ENOMEM && (hold_lease || waiting_observed) &&
		    run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && fixture.calls == 1 &&
		    (!hold_lease || fixture.lease_acquired) && static_cast<int32_t>(state.gprs[0]) == expected &&
		    state.rip == root && state.gprs[4] == fixture.stack.address + 2 * UNIT && record_correct &&
		    after.try_denials == before.try_denials + 1 + (hold_lease ? 1 : 0) && cleanup &&
		    final_state.active == 0 && final_state.waiting == 0;
		std::printf("KernelVirtualRangeTest: quiescent-reserve case=%s waiting=%u mutation=%u run=%u result=0x%08x pass=%u\n",
		            hold_lease ? "held-lease-denied" : free_output ? "output-unmapped" : "contended-success",
		            waiting_observed, mutation_ok, run_result, static_cast<uint32_t>(state.gprs[0]), passed);
		if (!Check(passed, "quiescent lifecycle reservation result or cleanup differs")) return false;
	}
	return true;
}

bool TestCompetingExclusiveReservation()
{
	// This uses the active FEX runtime, its real HLE reader, and Core's real
	// reservation transaction. The writer must pass both HLE mutexes while
	// Reserve is paused, and can revoke/change its output before readmission.
	enum class Case { Success, RequestedChanged, OutputUnmapped, HeldLease };
	for (const auto scenario: {Case::Success, Case::RequestedChanged, Case::OutputUnmapped, Case::HeldLease})
	{
		constexpr size_t offset = 53;
		LifecycleFexRun fixture;
		GuestPages record;
		if (!Check(fixture.code.address != 0 && fixture.stack.address != 0 && record.address != 0,
		           "competing reserve fixture allocation failed")) return false;
		fixture.competing_writer = true;
		fixture.hold_lease = scenario == Case::HeldLease;
		record.Set(offset, 0);
		std::array<uint8_t, 2 * UNIT> before_record;
		std::memcpy(before_record.data(), record.Data(), before_record.size());
		std::array<uint8_t, 13> program {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0xc3};
		const uint64_t target = reinterpret_cast<uint64_t>(g_reserve);
		std::memcpy(program.data() + 2, &target, sizeof(target));
		std::memcpy(fixture.code.Data(), program.data(), program.size());
		if (!Check(Vm::Protect(fixture.code.address, 2 * UNIT, Vm::Mode::ExecuteRead) &&
		               LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1,
		           "competing reserve fixture publication failed")) return false;
		const auto transactions_before = Vm::GetVirtualRangeTransactionStateForTest();
		const uint64_t generation_before = Vm::GetMutationGeneration();
		const uint64_t record_address = record.address;
		bool writer_acquired = false, writer_closed = false, locks_released = false;
		bool record_mutated = false, reserve_returned_before_writer = false;
		std::thread writer([&] {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (!fixture.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			if (!fixture.entered.load(std::memory_order_acquire)) return;
			const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, nullptr);
			if (token == 0) return;
			writer_acquired = true;
			reserve_returned_before_writer = fixture.reserve_returned.load(std::memory_order_acquire);
			// GetVirtualRangeState takes the actual owner mutex. The active
			// lifecycle count must be zero before this writer accesses guest data.
			const auto owners = Memory::GetVirtualRangeStateForTest();
			locks_released = owners.live == 0 && owners.pending_cleanup == 0 &&
			    Memory::GetMemoryLifecycleStateForTest().active == 0;
			if (scenario == Case::OutputUnmapped) record_mutated = Vm::Free(record_address);
			else if (scenario == Case::RequestedChanged)
			{
				Vm::GuestMemoryMutationOwnerScope owner(token);
				const uint64_t changed = 1;
				record_mutated = owner.IsActive() && Lumen::Loader::WriteGuestMemory(
				    record_address + offset, &changed, sizeof(changed));
			}
			else record_mutated = true;
			writer_closed = LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
		});
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = fixture.code.address;
		state.gprs[4] = fixture.stack.address + 2 * UNIT - sizeof(uint64_t);
		state.gprs[7] = reinterpret_cast<uint64_t>(record.Record(offset));
		state.gprs[6] = UNIT;
		state.gprs[1] = UNIT;
		const uint64_t root = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root, sizeof(root));
		const auto run_result = LumenAndroidFexCoreRunGuestV1(&state, LifecycleFexHostCall, QueryLifecycleFexCode, &fixture);
		writer.join();
		if (scenario == Case::OutputUnmapped && record_mutated) record.address = 0;
		const int expected = scenario == Case::Success ? 0 : scenario == Case::RequestedChanged ? Kernel::KERNEL_ERROR_EINVAL :
		    scenario == Case::OutputUnmapped ? Kernel::KERNEL_ERROR_EFAULT : Kernel::KERNEL_ERROR_ENOMEM;
		const uint64_t expected_transactions = scenario == Case::Success ? 1 : 0;
		const auto transactions_after = Vm::GetVirtualRangeTransactionStateForTest();
		const auto owners = Memory::GetVirtualRangeStateForTest();
		Vm::ManagedRegion region {};
		const bool record_correct = scenario == Case::OutputUnmapped
		    ? Vm::QueryManagedRegion(record_address, &region) == Vm::ManagedRegionState::Unmanaged
		    : OutsideUnchanged(record, offset, before_record) &&
		      (scenario == Case::Success ? record.Get(offset) != 0 &&
		          Vm::QueryManagedRegion(record.Get(offset), &region) == Vm::ManagedRegionState::Reserved
		       : record.Get(offset) == (scenario == Case::RequestedChanged ? 1 : 0));
		const uint64_t expected_mutations = scenario == Case::Success || scenario == Case::OutputUnmapped ? 1 : 0;
		const bool passed = writer_acquired && writer_closed && locks_released && record_mutated && fixture.conflict_observed &&
		    reserve_returned_before_writer == (scenario == Case::HeldLease) &&
		    (!fixture.hold_lease || fixture.lease_acquired) && run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
		    fixture.calls == 1 && static_cast<int32_t>(state.gprs[0]) == expected && state.rip == root &&
		    state.gprs[4] == fixture.stack.address + 2 * UNIT && record_correct &&
		    transactions_after.begins == transactions_before.begins + expected_transactions &&
		    transactions_after.closes == transactions_before.closes + expected_transactions &&
		    transactions_after.active == 0 && Vm::GetMutationGeneration() == generation_before + expected_mutations &&
		    owners.live == expected_transactions && owners.pending_cleanup == 0;
		const bool cleanup = Memory::ResetForNextSession();
		const auto lifecycle = Memory::GetMemoryLifecycleStateForTest();
		std::printf("KernelVirtualRangeTest: competing-reserve case=%u conflict=%u writer=%u locks-released=%u"
		            " result=0x%08x transactions=%llu pass=%u\n", static_cast<unsigned>(scenario), fixture.conflict_observed,
		            writer_acquired && writer_closed, locks_released, static_cast<uint32_t>(state.gprs[0]),
		            static_cast<unsigned long long>(transactions_after.begins - transactions_before.begins), passed);
		if (!Check(passed && cleanup && lifecycle.active == 0 && lifecycle.waiting == 0 &&
		               Memory::GetVirtualRangeStateForTest().live == 0,
		           "competing reservation retry, revalidation, terminal denial or cleanup differs")) return false;
	}
	return true;
}

bool TestFailureTransactions(GuestPages& pages)
{
	// Reused caller storage must not preserve a stray retry flag on a
	// terminal failure, nor on successful admission.
	for (const auto fault: {Vm::VirtualRangeFaultForTest::None, Vm::VirtualRangeFaultForTest::FexAdditiveAdmission,
	                       Vm::VirtualRangeFaultForTest::CoreCapacity})
	{
		Vm::AllocationDenialMetadata denial {Vm::AllocationFailure::AdmissionDenied, true};
		Vm::ReservationHandle handle {};
		Vm::MutationTransaction transaction;
		const auto generation = Vm::GetMutationGeneration();
		Vm::FailNextVirtualRangeOperationForTest(fault);
		const bool success = Vm::BeginReserveAligned(0, fault == Vm::VirtualRangeFaultForTest::None ? 0 : UNIT, UNIT,
		    &handle, &transaction, Vm::ReservationPlacement::Exact, &denial);
		const auto expected_reason = fault == Vm::VirtualRangeFaultForTest::None ? Vm::AllocationFailure::InvalidArgument :
		    fault == Vm::VirtualRangeFaultForTest::FexAdditiveAdmission ? Vm::AllocationFailure::AdmissionDenied :
		    Vm::AllocationFailure::HostAllocationFailed;
		if (!Check(!success && !denial.retryable && denial.reason == expected_reason && !handle.IsValid() &&
		               !transaction.IsActive() && Vm::GetMutationGeneration() == generation,
		           "terminal reservation metadata retained retry permission or an effect")) return false;
	}
	Vm::AllocationDenialMetadata success_denial {Vm::AllocationFailure::AdmissionDenied, true};
	Vm::ReservationHandle metadata_handle {};
	Vm::MutationTransaction metadata_transaction;
	if (!Check(Vm::BeginReserveAligned(0, UNIT, UNIT, &metadata_handle, &metadata_transaction,
	               Vm::ReservationPlacement::Exact, &success_denial) && success_denial.reason == Vm::AllocationFailure::None &&
	               !success_denial.retryable && metadata_transaction.Abort(),
	           "successful reservation retained stale denial metadata")) return false;
	constexpr size_t offset = 512;
	pages.Set(offset, 0);
	const uint64_t before_slot = Vm::GetMutationGeneration();
	Memory::FailNextVirtualRangeOperationForTest(Memory::VirtualRangeFaultForTest::HleSlotPreflight);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_ENOMEM && pages.Get(offset) == 0 &&
	               Vm::GetMutationGeneration() == before_slot,
	           "HLE slot preflight failure mutated state"))
	{
		return false;
	}

	const uint64_t before_core_capacity = Vm::GetMutationGeneration();
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::CoreCapacity);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_ENOMEM && pages.Get(offset) == 0 &&
	               Vm::GetMutationGeneration() == before_core_capacity,
	           "Core capacity failure mutated HLE or VM state"))
	{
		return false;
	}
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::FexAdditiveAdmission);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_ENOMEM && pages.Get(offset) == 0 &&
	               Vm::GetMutationGeneration() == before_core_capacity,
	           "FEX additive denial mutated HLE or VM state"))
	{
		return false;
	}

	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::SnapshotPublish);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == 0, "snapshot publication fallback rejected reservation"))
	{
		return false;
	}
	const uint64_t    snapshot_base = pages.Get(offset);
	Vm::ManagedRegion snapshot_region {};
	if (!Check(Vm::QueryManagedRegion(snapshot_base, &snapshot_region) == Vm::ManagedRegionState::Reserved,
	           "snapshot failure lost authoritative fallback") ||
	    !Check(Memory::ResetForNextSession(), "snapshot-fallback reservation cleanup failed"))
	{
		return false;
	}
	pages.Set(offset, 0);

	const uint64_t before_write = Vm::GetMutationGeneration();
	Memory::FailNextVirtualRangeOperationForTest(Memory::VirtualRangeFaultForTest::FinalGuestWrite);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT && pages.Get(offset) == 0 &&
	               Vm::GetMutationGeneration() == before_write + 2,
	           "final write failure did not perform one-gate rollback"))
	{
		return false;
	}
	auto state = Memory::GetVirtualRangeStateForTest();
	if (!Check(state.live == 0 && state.pending_cleanup == 0, "successful Abort retained an HLE owner"))
	{
		return false;
	}

	Memory::FailNextVirtualRangeOperationForTest(Memory::VirtualRangeFaultForTest::FinalGuestWrite);
	Vm::FailNextVirtualRangeOperationForTest(Vm::VirtualRangeFaultForTest::AbortHost);
	if (!Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_EFAULT && pages.Get(offset) == 0,
	           "failed Abort changed the guest error or output"))
	{
		return false;
	}
	state                          = Memory::GetVirtualRangeStateForTest();
	const uint64_t    pending_base = state.first_base;
	Vm::ManagedRegion region {};
	if (!Check(state.live == 1 && state.pending_cleanup == 1 && pending_base != 0 &&
	               Vm::QueryManagedRegion(pending_base, &region) == Vm::ManagedRegionState::Reserved,
	           "failed Abort lost its pending owner") ||
	    !Check(Reserve(pages.Record(offset), UNIT, 0, UNIT) == Kernel::KERNEL_ERROR_ENOMEM && pages.Get(offset) == 0,
	           "pending cleanup did not block a later reservation") ||
	    !Check(Memory::ResetForNextSession(), "pending owner retry cleanup failed"))
	{
		return false;
	}
	state = Memory::GetVirtualRangeStateForTest();
	return Check(state.live == 0 && state.pending_cleanup == 0 &&
	                 Vm::QueryManagedRegion(pending_base, &region) == Vm::ManagedRegionState::Unmanaged,
	             "retry cleanup retained Core or HLE state");
}
#endif

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2 || (std::strcmp(argv[1], "--snapshot-enabled") != 0 && std::strcmp(argv[1], "--snapshot-disabled") != 0))
	{
		std::fprintf(stderr, "usage: KernelVirtualRangeTest --snapshot-enabled|--snapshot-disabled\n");
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
	bool ok = Check(Vm::IsImmutableRegionSnapshotEnabled() == expected_snapshot, "snapshot selector oracle differs") &&
	          Check(pages.address != 0, "guest record fixture allocation failed") && TestBinding(&symbols) &&
	          TestScalarAndPointerErrors(pages) && TestProtectedRecords(pages) && TestSuccessCollisionAndReset(pages) &&
	          TestDirectMappingValidation(pages) && TestReservedDirectMapping(pages) && TestOwnedPhysicalBackingHandoff(pages) &&
	          TestPhysicalBackingHandoff(pages) && TestReservedFlexibleMapping(pages);
#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK) && defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	ok = ok && TestOwnerIdentityDuringCoreRelease() && TestMappingFailureTransactions(pages) && TestFailureTransactions(pages) && TestLifecycleCallbackOrdering(pages) &&
	     TestQuiescentLifecycleReservation() && TestCompetingExclusiveReservation();
#endif
	if (!Check(!g_bridge_failed, "scalar host-call bridge failed") || !ok)
	{
		return 1;
	}
	std::puts("KernelVirtualRangeTest: PASS");
	return 0;
}
