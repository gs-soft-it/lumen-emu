#include "Lumen/Core/VirtualMemory.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

namespace VirtualMemory = Lumen::Core::VirtualMemory;

constexpr uint64_t ORBIS_GRANULARITY = UINT64_C(0x4000);
constexpr uint64_t PREFERRED_BASE = UINT64_C(0x1000000000);

bool Check(bool condition, const char* step)
{
	if (!condition)
	{
		std::fprintf(stderr, "windows-virtual-range failure=%s error=0x%08lx\n",
		             step, static_cast<unsigned long>(GetLastError()));
	}
	return condition;
}

bool QueryState(uint64_t address, DWORD expected)
{
	MEMORY_BASIC_INFORMATION region {};
	return VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
	                    &region, sizeof(region)) == sizeof(region) &&
	       region.State == expected;
}

bool QueryProtection(uint64_t address, DWORD expected)
{
	MEMORY_BASIC_INFORMATION region {};
	return VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
	                    &region, sizeof(region)) == sizeof(region) &&
	       region.State == MEM_COMMIT && (region.Protect & 0xffu) == expected;
}

bool TestValidation(uint64_t page_size)
{
	const uint64_t generation = VirtualMemory::GetMutationGeneration();
	VirtualMemory::ReservationHandle handle {1, 1, 1};
	VirtualMemory::MutationTransaction transaction;
	const bool rejected = !VirtualMemory::BeginReserveAligned(
	    0, 4 * page_size, page_size, &handle, &transaction,
	    static_cast<VirtualMemory::ReservationPlacement>(UINT32_MAX));
	return Check(rejected && !handle.IsValid() && !transaction.IsActive() &&
	                 VirtualMemory::GetMutationGeneration() == generation,
	             "invalid-placement");
}

bool TestReservationExtent(uint64_t page_size)
{
	const uint64_t size = 3 * ORBIS_GRANULARITY;
	VirtualMemory::ReservationHandle handle {};
	VirtualMemory::MutationTransaction transaction;
	if (!Check(VirtualMemory::BeginReserveAligned(0, size, ORBIS_GRANULARITY,
	                                              &handle, &transaction),
	           "extent-reserve"))
	{
		return false;
	}
	transaction.Commit();
	const bool ok = Check(handle.IsValid() && handle.base % ORBIS_GRANULARITY == 0 &&
	                          QueryState(handle.base, MEM_RESERVE) &&
	                          QueryState(handle.base + size - page_size, MEM_RESERVE) &&
	                          QueryState(handle.base + size, MEM_FREE),
	                      "extent-no-padding");
	return Check(VirtualMemory::ReleaseReservation(handle), "extent-release") && ok;
}

bool TestAbort(uint64_t page_size)
{
	VirtualMemory::ReservationHandle handle {};
	VirtualMemory::MutationTransaction transaction;
	if (!Check(VirtualMemory::BeginReserveAligned(0, 4 * page_size, page_size,
	                                              &handle, &transaction),
	           "abort-reserve"))
	{
		return false;
	}
	const uint64_t base = handle.base;
	return Check(transaction.Abort() && !transaction.IsActive() &&
	                 QueryState(base, MEM_FREE) &&
	                 !VirtualMemory::ReleaseReservation(handle),
	             "abort-rollback");
}

bool TestLifecycle(uint64_t page_size, uint64_t allocation_granularity)
{
	constexpr uint64_t parent_size = UINT64_C(2) * 1024 * 1024 * 1024;
	VirtualMemory::ReservationHandle parent {};
	VirtualMemory::MutationTransaction reserve;
	if (!Check(VirtualMemory::BeginReserveAligned(
	               PREFERRED_BASE, parent_size, allocation_granularity,
	               &parent, &reserve, VirtualMemory::ReservationPlacement::PreferAddress),
	           "parent-reserve"))
	{
		return false;
	}
	reserve.Commit();
	if (!Check(parent.IsValid() && parent.base % allocation_granularity == 0 &&
	               QueryState(parent.base, MEM_RESERVE) &&
	               VirtualMemory::HasReservationOverlap(parent.base, page_size),
	           "parent-state"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}

	VirtualMemory::ReservationHandle fallback {};
	VirtualMemory::MutationTransaction fallback_transaction;
	if (!Check(VirtualMemory::BeginReserveAligned(
	               parent.base, allocation_granularity, allocation_granularity,
	               &fallback, &fallback_transaction,
	               VirtualMemory::ReservationPlacement::PreferAddress),
	           "preferred-fallback"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}
	fallback_transaction.Commit();
	const bool fallback_ok = Check(fallback.base != parent.base &&
	                                   QueryState(parent.base, MEM_RESERVE) &&
	                                   QueryState(fallback.base, MEM_RESERVE),
	                               "preferred-no-clobber") &&
	                         Check(VirtualMemory::ReleaseReservation(fallback),
	                               "preferred-fallback-release");
	if (!fallback_ok)
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}

	const uint64_t child = parent.base + 2 * ORBIS_GRANULARITY;
	const uint64_t child_size = 2 * ORBIS_GRANULARITY;
	if (!Check(!VirtualMemory::Free(parent.base + page_size) &&
	               QueryState(parent.base, MEM_RESERVE),
	           "hole-free-denied"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}

	VirtualMemory::MutationTransaction activate;
	if (!Check(VirtualMemory::BeginAllocFixedInReservation(
	               child, child_size, VirtualMemory::Mode::ReadWrite, &activate) ==
	               VirtualMemory::ReservedFixedResult::Begun,
	           "child-activate"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}
	activate.Commit();
	std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(child)), 0xa5,
	            static_cast<size_t>(child_size));

	VirtualMemory::MutationTransaction overlap;
	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	const bool guarded = Check(
	    VirtualMemory::BeginAllocFixedInReservation(
	        child + page_size, page_size, VirtualMemory::Mode::ReadWrite, &overlap) ==
	        VirtualMemory::ReservedFixedResult::Failed &&
	        !overlap.IsActive() &&
	        !VirtualMemory::Protect(child - page_size, 2 * page_size,
	                                VirtualMemory::Mode::Read, &old_mode),
	    "child-confinement");
	if (!guarded ||
	    !Check(VirtualMemory::Protect(child + page_size, page_size,
	                                  VirtualMemory::Mode::Read, &old_mode) &&
	               old_mode == VirtualMemory::Mode::ReadWrite &&
	               QueryProtection(child + page_size, PAGE_READONLY) &&
	               VirtualMemory::Protect(child + page_size, page_size,
	                                      VirtualMemory::Mode::ReadWrite),
	           "child-protect"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}

	if (!Check(VirtualMemory::Free(child) && QueryState(child, MEM_RESERVE) &&
	               QueryState(parent.base, MEM_RESERVE),
	           "child-decommit") ||
	    !Check(VirtualMemory::AllocFixed(child, child_size,
	                                     VirtualMemory::Mode::ReadWrite),
	           "child-reactivate"))
	{
		(void)VirtualMemory::ReleaseReservation(parent);
		return false;
	}
	const auto* bytes = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(child));
	for (uint64_t index = 0; index < child_size; index++)
	{
		if (!Check(bytes[index] == 0, "child-zero-fill"))
		{
			(void)VirtualMemory::ReleaseReservation(parent);
			return false;
		}
	}

	const VirtualMemory::ReservationHandle stale = parent;
	if (!Check(VirtualMemory::ReleaseReservation(parent) &&
	               QueryState(stale.base, MEM_FREE),
	           "parent-release"))
	{
		return false;
	}

	VirtualMemory::ReservationHandle replacement {};
	VirtualMemory::MutationTransaction exact;
	if (!Check(VirtualMemory::BeginReserveAligned(
	               stale.base, stale.size, allocation_granularity,
	               &replacement, &exact, VirtualMemory::ReservationPlacement::Exact),
	           "exact-reuse"))
	{
		return false;
	}
	exact.Commit();
	const bool identity_ok = Check(replacement.identity != stale.identity &&
	                                   !VirtualMemory::ReleaseReservation(stale) &&
	                                   QueryState(replacement.base, MEM_RESERVE),
	                               "stale-handle");
	return Check(VirtualMemory::ReleaseReservation(replacement),
	             "replacement-release") && identity_ok;
}

} // namespace

int main()
{
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	const uint64_t page_size = static_cast<uint64_t>(info.dwPageSize);
	const uint64_t allocation_granularity =
	    static_cast<uint64_t>(info.dwAllocationGranularity);
	if (!Check(page_size != 0 && allocation_granularity >= page_size &&
	               allocation_granularity % page_size == 0,
	           "system-granularity"))
	{
		return 1;
	}

	VirtualMemory::Init();
	const bool validation = TestValidation(page_size);
	const bool extent = TestReservationExtent(page_size);
	const bool abort = TestAbort(page_size);
	const bool lifecycle = TestLifecycle(page_size, allocation_granularity);
	std::printf("windows-virtual-range page=%llu allocation-granularity=%llu "
	            "validation=%s extent=%s abort=%s lifecycle=%s\n",
	            static_cast<unsigned long long>(page_size),
	            static_cast<unsigned long long>(allocation_granularity),
	            validation ? "ok" : "fail", extent ? "ok" : "fail",
	            abort ? "ok" : "fail", lifecycle ? "ok" : "fail");
	return validation && extent && abort && lifecycle ? 0 : 1;
}
