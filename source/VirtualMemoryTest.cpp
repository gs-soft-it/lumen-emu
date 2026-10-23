#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Core/VirtualMemoryImmutableSnapshot.h"

#include "cpu/runtime/MemoryHandoff.h"
#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace VirtualMemory = Lumen::Core::VirtualMemory;

constexpr uint64_t GUEST_PAGE_SIZE = 4096;

struct Region
{
	vm_address_t address = 0;
	vm_size_t    size    = 0;
	vm_prot_t    current = VM_PROT_NONE;
};

bool QueryRegion(uint64_t address, Region* region)
{
	if (region == nullptr)
	{
		return false;
	}

	vm_address_t                   region_address = static_cast<vm_address_t>(address);
	vm_size_t                      region_size    = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t         info_count  = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t                    object_name = MACH_PORT_NULL;
	const kern_return_t            result      = vm_region_64(mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64,
	                                                          reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
	if (object_name != MACH_PORT_NULL)
	{
		mach_port_deallocate(mach_task_self(), object_name);
	}
	if (result != KERN_SUCCESS || region_address > address || region_size == 0 || address - region_address >= region_size)
	{
		return false;
	}

	region->address = region_address;
	region->size    = region_size;
	region->current = info.protection;
	return true;
}

bool HasProtection(uint64_t address, vm_prot_t required, vm_prot_t forbidden)
{
	Region region {};
	return QueryRegion(address, &region) && (region.current & required) == required && (region.current & forbidden) == 0;
}

bool TestSharedPhysicalBacking(uint64_t page_size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace VirtualMemory;
	char name[64] {};
	std::snprintf(name, sizeof(name), "/lm-vmtest-%x", static_cast<unsigned>(getpid()));
	const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return false;
	const bool unlinked = shm_unlink(name) == 0;
	struct CloseBacking { int fd; ~CloseBacking() { close(fd); } } backing {fd};
	if (!unlinked || ftruncate(fd, static_cast<off_t>(2 * page_size)) != 0) return false;
	const uint64_t first = Alloc(0, page_size, Mode::ReadWrite);
	const uint64_t alias = Alloc(0, page_size, Mode::ReadWrite);
	if (first == 0 || alias == 0) return false;
	struct ReleaseMapping { uint64_t address; ~ReleaseMapping() { if (address != 0) Free(address); } } first_cleanup {first}, alias_cleanup {alias};
	// A native Core borrower admitted after anonymous allocation must also
	// finish before MAP_FIXED replaces the fresh backing. It is not a FEX
	// execution reader, so this specifically exercises the local VM gate.
	*reinterpret_cast<uint8_t*>(first) = 0x2a;
	GuestDataLease pin;
	if (!TryAcquireGuestDataLease(first, 1, Mode::Read, &pin)) return false;
	std::atomic<bool> binding_started {false}, binding_complete {false};
	bool binding_result = false;
	std::thread binder([&] {
		binding_started = true;
		binding_result = BindFreshGuestMappingToSharedBacking(first, page_size, fd, page_size, Mode::ReadWrite);
		binding_complete = true;
	});
	while (!binding_started.load()) std::this_thread::yield();
	bool pin_excludes_binding = false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (GetVirtualRangeTransactionStateForTest().waiting_mutations == 0 &&
	       !binding_complete.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	pin_excludes_binding = GetVirtualRangeTransactionStateForTest().waiting_mutations == 1 &&
	    !binding_complete.load() && *reinterpret_cast<const uint8_t*>(pin.HostBegin()) == 0x2a;
#else
	pin_excludes_binding = !binding_complete.load();
#endif
	ReleaseGuestDataLease(&pin);
	binder.join();
	if (!pin_excludes_binding || !binding_result ||
	    !BindFreshGuestMappingToSharedBacking(alias, page_size, fd, page_size, Mode::ReadWrite)) return false;
	auto* first_bytes = reinterpret_cast<uint8_t*>(first);
	auto* alias_bytes = reinterpret_cast<uint8_t*>(alias);
	first_bytes[17] = 0x91;
	if (alias_bytes[17] != 0x91 || !Protect(first, page_size, Mode::Read)) return false;
	alias_bytes[17] = 0x52;
	if (first_bytes[17] != 0x52 || !HasProtection(first, VM_PROT_READ, VM_PROT_WRITE) ||
	    !Protect(first, page_size, Mode::NoAccess)) return false;
	alias_bytes[17] = 0xb4;
	GuestDataLease denied;
	if (TryAcquireGuestDataLease(first + 17, 1, Mode::Read, &denied, false) ||
	    !HasProtection(first, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE)) return false;
	if (!Free(first)) return false;
	first_cleanup.address = 0;
	const uint64_t remap = Alloc(0, page_size, Mode::Read);
	ReleaseMapping remap_cleanup {remap};
	if (remap == 0 || !BindFreshGuestMappingToSharedBacking(remap, page_size, fd, page_size, Mode::Read) ||
	    reinterpret_cast<const uint8_t*>(remap)[17] != 0xb4) return false;
	// The binding must reject partial/unaligned/out-of-file requests without
	// changing any accepted mapping's contents or rights.
	if (BindFreshGuestMappingToSharedBacking(alias, page_size / 2, fd, 0, Mode::ReadWrite) ||
	    BindFreshGuestMappingToSharedBacking(alias, page_size, fd, 1, Mode::ReadWrite) ||
	    BindFreshGuestMappingToSharedBacking(alias, page_size, fd, 2 * page_size, Mode::ReadWrite) || alias_bytes[17] != 0xb4) return false;
	ReservationHandle reservation;
	MutationTransaction reserve;
	if (!BeginReserveAligned(0, page_size, page_size, &reservation, &reserve)) return false;
	reserve.Commit();
	struct ReleaseParent { ReservationHandle handle; ~ReleaseParent() { ReleaseReservation(handle); } } parent {reservation};
	MutationTransaction child;
	if (BeginAllocFixedInReservation(reservation.base, page_size, Mode::ReadWrite, &child) != ReservedFixedResult::Begun ||
	    !BindFreshGuestMappingToSharedBacking(reservation.base, page_size, fd, page_size, Mode::ReadWrite)) return false;
	reinterpret_cast<uint8_t*>(reservation.base)[17] = 0xc7;
	if (!child.Abort() || alias_bytes[17] != 0xc7) return false;
	ManagedRegion region;
	if (QueryManagedRegion(reservation.base, &region) != ManagedRegionState::Reserved) return false;
	MutationTransaction next;
	if (BeginAllocFixedInReservation(reservation.base, page_size, Mode::Read, &next) != ReservedFixedResult::Begun ||
	    !BindFreshGuestMappingToSharedBacking(reservation.base, page_size, fd, page_size, Mode::Read)) return false;
	next.Commit();
	return reinterpret_cast<const uint8_t*>(reservation.base)[17] == 0xc7 && Free(reservation.base) &&
	    alias_bytes[17] == 0xc7 && QueryManagedRegion(reservation.base, &region) == ManagedRegionState::Reserved;
#else
	(void)page_size;
	return true;
#endif
}

bool TestExternalHandoffOwner(uint64_t page_size)
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	using namespace VirtualMemory;
	const uint64_t mapping = Alloc(0, page_size, Mode::ReadWrite);
	if (mapping == 0) return false;
	struct ReleaseMapping { uint64_t address; ~ReleaseMapping() { Free(address); } } cleanup {mapping};
	const auto begin = [](uint32_t kind) { return LumenAndroidFexCoreBeginGuestMemoryMutationV2(kind); };
	const auto end = [](uint64_t token) { return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1; };
	const uint64_t additive = begin(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2);
	if (additive == 0) return false;
	bool ok = true;
	{ GuestMemoryMutationOwnerScope rejected(additive); ok = !rejected.IsActive(); }
	if (!end(additive) || !ok) return false;
	const uint64_t token = begin(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
	if (token == 0) return false;
	{ GuestMemoryMutationOwnerScope invalid(token + 1); ok = !invalid.IsActive() && ok; }
	std::atomic<bool> completion_ready {false}, release_completion {false};
	bool completion_ok = false;
	std::thread completion;
	GuestDataRangeSeal producer_seal;
	{
		GuestMemoryMutationOwnerScope owner(token);
		ok = owner.IsActive() && !CurrentThreadHasMemoryLease() && ok;
		if (owner.IsActive())
		{
			// A producer cannot recursively mutate mappings protected by its
			// own local pin. This must fail promptly, rather than self-deadlock.
			ok = !Protect(mapping, page_size, Mode::Read) && ok;
			GuestDataLease outer, nested;
			ok = TryAcquireGuestDataLease(mapping + 16, 8, Mode::ReadWrite, &outer, false) && ok;
			ok = TryAcquireGuestDataLease(mapping + 20, 4, Mode::Read, &nested, false) && ok;
			ok = ValidateGuestDataLeaseRangeContinuity(outer, mapping + 16, 8, outer.MutationGeneration()) &&
			    CaptureGuestDataRangeSeal(outer, &producer_seal) && ValidateGuestDataRangeSeal(outer, producer_seal) && ok;
			ok = !ValidateGuestDataLeaseRangeContinuity(outer, mapping + 17, 8, outer.MutationGeneration()) && ok;
			GuestMemoryMutationOwnerScope duplicate(token);
			ok = !duplicate.IsActive() && CurrentThreadHasMemoryLease() && ok;
			completion = std::thread([&] {
				GuestMemoryMutationOwnerScope wrong_thread(token);
				GuestDataLease write;
				GuestDataLeaseDenialMetadata denial;
				completion_ok = !wrong_thread.IsActive() &&
				    TryAcquireGuestDataLease(mapping + 32, 8, Mode::Write, &write, false, &denial) &&
				    denial.reason == GuestDataLeaseDenialReason::None &&
				    ValidateGuestDataLeaseRangeContinuity(write, mapping + 32, 8, write.MutationGeneration());
				if (completion_ok)
				{
					const uint64_t value = UINT64_C(0x12345678abcdef90);
					std::memcpy(reinterpret_cast<void*>(write.HostBegin()), &value, sizeof(value));
				}
				completion_ready = true;
				while (!release_completion.load()) std::this_thread::yield();
			});
			while (!completion_ready.load()) std::this_thread::yield();
			// Concurrent Core mutation cannot acquire FEX while our producer
			// epoch remains live, even though native completion borrowing works.
			bool foreign_mutation = true;
			std::thread mutator([&] { foreign_mutation = Protect(mapping, page_size, Mode::Read); });
			mutator.join();
			ok = !foreign_mutation && completion_ok && ok;
			ReleaseGuestDataLease(&nested);
			ReleaseGuestDataLease(&outer);
			ok = !CurrentThreadHasMemoryLease() && ok;
		}
	}
	// Remove only the producer pin while the other thread still holds its
	// native completion lease, then readmit FEX. Existing storage is unchanged.
	ok = end(token) && ok;
	{ GuestMemoryMutationOwnerScope stale(token); ok = !stale.IsActive() && ok; }
	{
		GuestDataLease unchanged;
		ok = TryAcquireGuestDataLease(mapping + 16, 8, Mode::ReadWrite, &unchanged) &&
		    ValidateGuestDataRangeSeal(unchanged, producer_seal) && ok;
	}
	std::atomic<bool> mutation_started {false}, mutation_completed {false};
	bool mutation_ok = false;
	std::thread mutator([&] {
		mutation_started = true;
		mutation_ok = Protect(mapping, page_size, Mode::Read);
		mutation_completed = true;
	});
	while (!mutation_started.load()) std::this_thread::yield();
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (GetVirtualRangeTransactionStateForTest().waiting_mutations == 0 &&
	       !mutation_completed.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	ok = GetVirtualRangeTransactionStateForTest().waiting_mutations == 1 && !mutation_completed.load() && ok;
	GuestDataLease pending;
	GuestDataLeaseDenialMetadata pending_denial;
	ok = !TryAcquireGuestDataLease(mapping, 8, Mode::Read, &pending, false, &pending_denial) &&
	    pending_denial.reason == GuestDataLeaseDenialReason::GateWriterPending && pending_denial.retryable && ok;
#endif
	release_completion = true;
	if (completion.joinable()) completion.join();
	mutator.join();
	ok = mutation_ok && Protect(mapping, page_size, Mode::ReadWrite) && ok;
	// A protection roundtrip must still revoke the original seal, even under
	// a later genuine producer epoch. Ownership cannot revive stale capture.
	const uint64_t next_token = begin(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
	if (next_token == 0) return false;
	{
		GuestMemoryMutationOwnerScope owner(next_token);
		GuestDataLease changed;
		GuestDataRangeSeal fresh_seal;
		ok = owner.IsActive() && TryAcquireGuestDataLease(mapping + 16, 8, Mode::ReadWrite, &changed, false) &&
		    !ValidateGuestDataRangeSeal(changed, producer_seal) && CaptureGuestDataRangeSeal(changed, &fresh_seal) &&
		    ValidateGuestDataRangeSeal(changed, fresh_seal) && ok;
	}
	ok = end(next_token) && ok;
	// Logical denial is never converted into admission contention/retry.
	ok = Protect(mapping, page_size, Mode::NoAccess) && ok;
	GuestDataLease denied;
	GuestDataLeaseDenialMetadata denial;
	ok = !TryAcquireGuestDataLease(mapping, 8, Mode::Read, &denied, false, &denial) && !denial.retryable &&
	    (denial.reason == GuestDataLeaseDenialReason::LogicalSnapshotDenied ||
	     denial.reason == GuestDataLeaseDenialReason::LogicalRegistryDenied) && ok;
	std::printf("virtual-memory external-owner shared-pin=%s native-completion=real nested-mutation=denied"
	    " FEX-mutation=excluded native-pin-after-owner=retained stale-seal=denied retry=gate-only\n", ok ? "ok" : "fail");
	return ok;
#else
	(void)page_size;
	return true;
#endif
}

bool AdvancedOnce(uint64_t before, uint64_t after)
{
	return before != (std::numeric_limits<uint64_t>::max)() && after == before + 1;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
bool DirectAccessDenied(uint64_t address, bool write)
{
	const pid_t child = fork();
	if (child < 0) return false;
	if (child == 0)
	{
		struct sigaction action {};
		action.sa_handler = [](int signal) { _exit(signal); };
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGSEGV, &action, nullptr) != 0 ||
		    sigaction(SIGBUS, &action, nullptr) != 0) _exit(1);
		auto* bytes = reinterpret_cast<volatile uint8_t*>(address);
		if (write) *bytes = 0x5a;
		else (void)*bytes;
		_exit(0);
	}
	int status = 0;
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       (WEXITSTATUS(status) == SIGSEGV || WEXITSTATUS(status) == SIGBUS);
}

bool TestUniformHostPageDataProtection(uint64_t host_page_size)
{
	if (!VirtualMemory::RequiresUniformHostPageDataProtection()) return true;
	if (host_page_size <= GUEST_PAGE_SIZE) return false;
	const uint64_t size = 3 * host_page_size;
	const uint64_t mapping = VirtualMemory::Alloc(0, size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0) return false;
	std::memset(reinterpret_cast<void*>(mapping), 0x31, size);
	bool ok = true;
	const auto unchanged = [&]
	{
		VirtualMemory::ManagedRegion region {};
		return VirtualMemory::QueryManagedRegion(mapping, &region) == VirtualMemory::ManagedRegionState::Visible &&
		       region.begin == mapping && region.end == mapping + size && region.mode == VirtualMemory::Mode::ReadWrite &&
		       HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) &&
		       HasProtection(mapping + host_page_size, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE);
	};
	for (uint64_t offset = 0; offset < host_page_size; offset += GUEST_PAGE_SIZE)
	{
		for (auto mode: {VirtualMemory::Mode::Read, VirtualMemory::Mode::ExecuteRead, VirtualMemory::Mode::NoAccess})
		{
			const uint64_t generation = VirtualMemory::GetMutationGeneration();
			VirtualMemory::Mode old = VirtualMemory::Mode::ExecuteWrite;
			const bool changed = VirtualMemory::Protect(mapping + offset, GUEST_PAGE_SIZE, mode, &old);
			ok = !changed && old == VirtualMemory::Mode::ExecuteWrite &&
			     generation == VirtualMemory::GetMutationGeneration() && unchanged() && ok;
		}
	}
	// A valid first host page followed by an unrepresentable final subpage
	// must fail before the first physical page or the snapshot is changed.
	const uint64_t generation = VirtualMemory::GetMutationGeneration();
	ok = !VirtualMemory::Protect(mapping, host_page_size + GUEST_PAGE_SIZE, VirtualMemory::Mode::Read) &&
	     generation == VirtualMemory::GetMutationGeneration() && unchanged() && ok;
	ok = VirtualMemory::Protect(mapping, host_page_size, VirtualMemory::Mode::Read) && ok;
	for (uint64_t offset = 0; offset < host_page_size; offset += GUEST_PAGE_SIZE)
	{
		ok = DirectAccessDenied(mapping + offset, true) && ok;
		ok = *reinterpret_cast<volatile uint8_t*>(mapping + offset) == 0x31 && ok;
	}
	// R and RX have the same data rights. Execution still requires the code
	// lease; the adjacent data-only guest page must never gain that lease.
	ok = VirtualMemory::Protect(mapping, GUEST_PAGE_SIZE, VirtualMemory::Mode::ExecuteRead) && ok;
	VirtualMemory::CodeReadLease code;
	VirtualMemory::CodeReadLease data;
	ok = VirtualMemory::TryAcquireCodeReadLease(mapping, 1, &code) &&
	     !VirtualMemory::TryAcquireCodeReadLease(mapping + GUEST_PAGE_SIZE, 1, &data) && ok;
	VirtualMemory::ReleaseCodeReadLease(&code);
	VirtualMemory::ReleaseCodeReadLease(&data);
	ok = !VirtualMemory::Protect(mapping + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, VirtualMemory::Mode::ReadWrite) && ok;
	ok = !VirtualMemory::Protect(mapping + GUEST_PAGE_SIZE, GUEST_PAGE_SIZE, VirtualMemory::Mode::NoAccess) && ok;
	// Loader relocation entries can still be patched without changing guest
	// rights, even with executable/read-only siblings in the same host page.
	const uint64_t plt = mapping + GUEST_PAGE_SIZE;
	const uint64_t first_pointer = UINT64_C(0x1122334455667788);
	const uint64_t second_pointer = UINT64_C(0x8877665544332211);
	ok = VirtualMemory::PatchReplace(plt + 8, first_pointer) &&
	     VirtualMemory::PatchReplace(plt + 16, second_pointer) &&
	     *reinterpret_cast<const uint64_t*>(plt + 8) == first_pointer &&
	     *reinterpret_cast<const uint64_t*>(plt + 16) == second_pointer &&
	     HasProtection(plt, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE) &&
	     VirtualMemory::TryAcquireCodeReadLease(mapping, 1, &code) && ok;
	VirtualMemory::ReleaseCodeReadLease(&code);
	ok = VirtualMemory::Protect(mapping + host_page_size, host_page_size, VirtualMemory::Mode::NoAccess) && ok;
	for (uint64_t offset = 0; offset < host_page_size; offset += GUEST_PAGE_SIZE)
		ok = DirectAccessDenied(mapping + host_page_size + offset, false) &&
		     DirectAccessDenied(mapping + host_page_size + offset, true) && ok;
	ok = HasProtection(mapping + 2 * host_page_size, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) && ok;
	ok = VirtualMemory::Free(mapping) && ok;

	// An allocation smaller than one host page remains useful. Its padding is
	// outside the visible mapping and is not an independent guest guard page.
	const uint64_t small = VirtualMemory::Alloc(0, GUEST_PAGE_SIZE, VirtualMemory::Mode::ReadWrite);
	if (small == 0) return false;
	VirtualMemory::ManagedRegion padding {};
	ok = VirtualMemory::Protect(small, GUEST_PAGE_SIZE, VirtualMemory::Mode::NoAccess) &&
	     DirectAccessDenied(small, false) &&
	     VirtualMemory::QueryManagedRegion(small + GUEST_PAGE_SIZE, &padding) == VirtualMemory::ManagedRegionState::Reserved &&
	     !VirtualMemory::IsManagedAddress(small + GUEST_PAGE_SIZE) && ok;
	return VirtualMemory::Free(small) && ok;
}

bool TestProtectionModeMatrix(uint64_t host_page_size)
{
	if (!VirtualMemory::RequiresUniformHostPageDataProtection()) return true;
	const uint64_t mapping = VirtualMemory::Alloc(0, 2 * host_page_size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0) return false;
	bool ok = true;
	// Independent x86-on-ARM data-access classes. Write-only and execute-only
	// guest modes need host read access as well; X remains a separate code gate.
	constexpr vm_prot_t data_rights[] = {
		VM_PROT_NONE, VM_PROT_READ, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
		VM_PROT_READ, VM_PROT_READ, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
	};
	for (uint32_t initial = 0; initial < 8; initial++)
	{
		for (uint32_t desired = 0; desired < 8; desired++)
		{
			for (uint64_t offset = 0; offset < host_page_size; offset += GUEST_PAGE_SIZE)
			{
				const auto initial_mode = static_cast<VirtualMemory::Mode>(initial);
				const auto desired_mode = static_cast<VirtualMemory::Mode>(desired);
				ok = VirtualMemory::Protect(mapping, 2 * host_page_size, initial_mode) && ok;
				const uint64_t generation = VirtualMemory::GetMutationGeneration();
				auto old = VirtualMemory::Mode::ExecuteWrite;
				const bool expected = data_rights[initial] == data_rights[desired];
				// A byte request rounds to its complete logical guest page.
				const bool changed = VirtualMemory::Protect(mapping + offset + 17, 1, desired_mode, &old);
				ok = changed == expected &&
				     old == (expected ? initial_mode : VirtualMemory::Mode::ExecuteWrite) &&
				     VirtualMemory::GetMutationGeneration() == generation + (expected ? 1 : 0) && ok;
				for (uint64_t page = 0; page < 2 * host_page_size; page += GUEST_PAGE_SIZE)
				{
					VirtualMemory::ManagedRegion region {};
					const auto wanted = expected && page == offset ? desired_mode : initial_mode;
					ok = VirtualMemory::QueryManagedRegion(mapping + page, &region) == VirtualMemory::ManagedRegionState::Visible &&
					     region.mode == wanted && ok;
					VirtualMemory::ManagedRegion snapshot {};
					auto state = VirtualMemory::ManagedRegionState::Unmanaged;
					if (VirtualMemory::IsImmutableRegionSnapshotEnabled())
						ok = VirtualMemory::TryQueryManagedRegionSnapshot(mapping + page, &state, &snapshot) &&
						     state == VirtualMemory::ManagedRegionState::Visible && snapshot.mode == wanted &&
						     snapshot.begin == region.begin && snapshot.end == region.end && ok;
				}
				ok = HasProtection(mapping, data_rights[initial],
				                   (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) & ~data_rights[initial]) && ok;
			}
		}
	}
	// Both affected host pages are partial. Rejection must preserve every page,
	// generation and the caller's output.
	ok = VirtualMemory::Protect(mapping, 2 * host_page_size, VirtualMemory::Mode::ReadWrite) && ok;
	const auto generation = VirtualMemory::GetMutationGeneration();
	auto old = VirtualMemory::Mode::ExecuteWrite;
	ok = !VirtualMemory::Protect(mapping + GUEST_PAGE_SIZE, 2 * host_page_size - 2 * GUEST_PAGE_SIZE,
	                             VirtualMemory::Mode::NoAccess, &old) &&
	     old == VirtualMemory::Mode::ExecuteWrite && generation == VirtualMemory::GetMutationGeneration() &&
	     HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) &&
	     HasProtection(mapping + host_page_size, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) && ok;
	return VirtualMemory::Free(mapping) && ok;
}
#else
bool TestUniformHostPageDataProtection(uint64_t)
{
	return !VirtualMemory::RequiresUniformHostPageDataProtection();
}

bool TestProtectionModeMatrix(uint64_t)
{
	return !VirtualMemory::RequiresUniformHostPageDataProtection();
}
#endif

bool TestInvalidProtectionModes(uint64_t host_page_size)
{
	const uint64_t mapping = VirtualMemory::Alloc(0, host_page_size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0) return false;
	bool ok = true;
	for (auto bits: {UINT32_C(8), UINT32_C(9), UINT32_MAX})
	{
		const auto invalid = static_cast<VirtualMemory::Mode>(bits);
		const auto generation = VirtualMemory::GetMutationGeneration();
		auto old = VirtualMemory::Mode::ExecuteWrite;
		VirtualMemory::MutationTransaction transaction;
		ok = VirtualMemory::Alloc(0, host_page_size, invalid) == 0 &&
		     VirtualMemory::AllocAligned(0, host_page_size, invalid, host_page_size) == 0 &&
		     !VirtualMemory::AllocFixed(mapping, host_page_size, invalid) &&
		     VirtualMemory::BeginAllocFixedInReservation(mapping, host_page_size, invalid, &transaction) ==
		         VirtualMemory::ReservedFixedResult::Failed && !transaction.IsActive() &&
		     !VirtualMemory::Protect(mapping, host_page_size, invalid, &old) &&
		     !VirtualMemory::Protect(mapping, 0, invalid, &old) &&
		     old == VirtualMemory::Mode::ExecuteWrite && generation == VirtualMemory::GetMutationGeneration() &&
		     HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) && ok;
	}
	return VirtualMemory::Free(mapping) && ok;
}

bool TestInitAndInvalid()
{
	VirtualMemory::Init();
	VirtualMemory::Init();

	VirtualMemory::Mode old_mode = VirtualMemory::Mode::ExecuteReadWrite;
	const bool          protect_overflow =
	    VirtualMemory::Protect((std::numeric_limits<uint64_t>::max)() - 7, 16, VirtualMemory::Mode::Read, &old_mode);
	return VirtualMemory::Alloc(0, 0, VirtualMemory::Mode::ReadWrite) == 0 &&
	       VirtualMemory::AllocAligned(0, 4096, VirtualMemory::Mode::ReadWrite, 0) == 0 &&
	       VirtualMemory::AllocAligned(0, 4096, VirtualMemory::Mode::ReadWrite, 3) == 0 &&
	       VirtualMemory::AllocAligned(0, (std::numeric_limits<uint64_t>::max)(), VirtualMemory::Mode::ReadWrite, 4096) == 0 &&
	       !VirtualMemory::AllocFixed(0, 4096, VirtualMemory::Mode::ReadWrite) && !VirtualMemory::Free(0) && !protect_overflow &&
	       old_mode == VirtualMemory::Mode::ExecuteReadWrite &&
	       !VirtualMemory::FlushInstructionCache((std::numeric_limits<uint64_t>::max)() - 3, 8);
}

bool TestGuestDataRangeContinuity(uint64_t page_size)
{
	using namespace VirtualMemory;
	const uint64_t bytes = 2 * page_size;
	uint64_t owned = Alloc(0, bytes, Mode::ReadWrite);
	uint64_t other = Alloc(0, page_size, Mode::ReadWrite);
	if (owned == 0 || other == 0) { if (owned != 0) Free(owned); if (other != 0) Free(other); return false; }
	struct Cleanup { uint64_t& owned; uint64_t& other; ~Cleanup() { if (owned != 0) Free(owned); if (other != 0) Free(other); } } cleanup {owned, other};
	std::memset(reinterpret_cast<void*>(owned), 0xa5, static_cast<size_t>(bytes));
	const auto observe = [&](uint64_t begin, uint64_t count, uint64_t baseline) {
		GuestDataLease lease;
		return TryAcquireGuestDataLease(begin, count, Mode::Read, &lease, false) &&
		       ValidateGuestDataLeaseRangeContinuity(lease, begin, count, baseline);
	};
	const uint64_t original_generation = GetMutationGeneration();
	if (!observe(owned + 8, 16, original_generation)) return false;
	{
		GuestDataLease empty;
		if (ValidateGuestDataLeaseRangeContinuity(empty, owned + 8, 16, original_generation)) return false;
		GuestDataLease active;
		if (!TryAcquireGuestDataLease(owned + 8, 16, Mode::Read, &active, false) ||
		    ValidateGuestDataLeaseRangeContinuity(active, owned + 9, 16, original_generation) ||
		    ValidateGuestDataLeaseRangeContinuity(active, owned + 8, 8, original_generation) ||
		    ValidateGuestDataLeaseRangeContinuity(active, owned + 8, 16, UINT64_MAX) ||
		    ValidateGuestDataLeaseRangeContinuity(active, owned + 8, 16, original_generation + 1)) return false;
		bool wrong_thread_denied = false;
		std::thread peer([&] {
			GuestDataLease own_lease;
			wrong_thread_denied = TryAcquireGuestDataLease(other, 8, Mode::Read, &own_lease, false) &&
			    !ValidateGuestDataLeaseRangeContinuity(active, owned + 8, 16, original_generation);
		});
		peer.join();
		if (!wrong_thread_denied) return false;
		ReleaseGuestDataLease(&active);
		if (ValidateGuestDataLeaseRangeContinuity(active, owned + 8, 16, original_generation)) return false;
	}
	{
		uint64_t native = 0;
		GuestDataLease lease;
		const uint64_t address = reinterpret_cast<uint64_t>(&native);
		if (!TryAcquireGuestDataLease(address, sizeof(native), Mode::Read, &lease, false) ||
		    ValidateGuestDataLeaseRangeContinuity(lease, address, sizeof(native), original_generation)) return false;
	}
	// Allocation, protection and freeing another map do not revoke this one.
	if (!Protect(other, page_size, Mode::Read) || !Protect(other, page_size, Mode::ReadWrite) ||
	    !observe(owned + 8, 16, original_generation)) return false;
	const uint64_t additive = Alloc(0, page_size, Mode::ReadWrite);
	if (additive == 0) return false;
	const bool additive_survived = observe(owned + 8, 16, original_generation);
	if (!Free(additive) || !additive_survived || !observe(owned + 8, 16, original_generation)) return false;
	// Mapping-wide conservatism rejects even a disjoint page in the same map.
	if (!Protect(owned + page_size, page_size, Mode::Read) ||
	    !Protect(owned + page_size, page_size, Mode::ReadWrite) || observe(owned + 8, 16, original_generation)) return false;
	uint64_t baseline = GetMutationGeneration();
	if (!observe(owned + 8, 16, baseline)) return false;
	uint64_t seed = 0;
	std::memcpy(&seed, reinterpret_cast<const void*>(owned + 8), sizeof(seed));
	if (!PatchReplace(owned + 8, seed ^ 1) || !PatchReplace(owned + 8, seed) || observe(owned + 8, 16, baseline)) return false;
	baseline = GetMutationGeneration();
	if (!observe(owned + 8, 16, baseline)) return false;
	// FEX invalidates even an unchanged patch attempt; other Apple paths keep
	// their public generation unchanged and use the conservative watermark.
	const bool fex_generation = RequiresUniformHostPageDataProtection();
	if (PatchReplace(owned + 8, seed) || GetMutationGeneration() != baseline + (fex_generation ? 1 : 0) ||
	    observe(owned + 8, 16, baseline) || !observe(other, 8, baseline)) return false;
	if (!Protect(owned, bytes, Mode::ReadWrite)) return false;
	baseline = GetMutationGeneration();
	if (!observe(owned + 8, 16, baseline)) return false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	// A real physical permission change rolled back before logical publication
	// still revokes the old continuity proof without changing public generation.
	const uint64_t other_baseline = baseline;
	FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest::ProtectAfterHost);
	if (Protect(owned, bytes, Mode::Read) || GetMutationGeneration() != baseline ||
	    !HasProtection(owned, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_NONE) ||
	    observe(owned + 8, 16, baseline) || !observe(other, 8, other_baseline)) return false;
	// A later unrelated generation permits a fresh capture but cannot revive
	// the original baseline that preceded the uncertain operation.
	if (!Protect(other, page_size, Mode::ReadWrite) || !observe(owned + 8, 16, GetMutationGeneration()) ||
	    observe(owned + 8, 16, baseline)) return false;
#endif
	// A footprint that cannot be bounded revokes every older range proof.
	baseline = GetMutationGeneration();
	if (FlushInstructionCache(UINT64_MAX - 3, 8) || !AdvancedOnce(baseline, GetMutationGeneration()) ||
	    observe(owned + 8, 16, baseline) || observe(other, 8, baseline) ||
	    !observe(owned + 8, 16, GetMutationGeneration())) return false;
	baseline = GetMutationGeneration();
	if (PatchReplace(UINT64_MAX - 3, 8) || GetMutationGeneration() != baseline + (fex_generation ? 1 : 0) ||
	    observe(owned + 8, 16, baseline) || observe(other, 8, baseline)) return false;
	if (!Protect(other, page_size, Mode::ReadWrite) || !observe(owned + 8, 16, GetMutationGeneration())) return false;
	// Replacing a mapping with identical bytes cannot restore its old identity.
	baseline = GetMutationGeneration();
	const uint64_t address = owned;
	if (!Free(owned)) return false;
	owned = 0;
	if (!AllocFixed(address, bytes, Mode::ReadWrite)) return false;
	owned = address;
	std::memset(reinterpret_cast<void*>(owned), 0xa5, static_cast<size_t>(bytes));
	if (observe(owned + 8, 16, baseline) || !observe(owned + 8, 16, GetMutationGeneration())) return false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	ReservationHandle reservation;
	MutationTransaction reserve;
	if (!BeginReserveAligned(0, bytes, page_size, &reservation, &reserve)) return false;
	{
		GuestDataLease owner_borrow;
		if (!TryAcquireGuestDataLease(owned + 8, 16, Mode::Read, &owner_borrow) ||
		    ValidateGuestDataLeaseRangeContinuity(owner_borrow, owned + 8, 16, GetMutationGeneration())) return false;
	}
	reserve.Commit();
	struct ReservationCleanup { ReservationHandle handle; ~ReservationCleanup() { ReleaseReservation(handle); } } reservation_cleanup {reservation};
	MutationTransaction activate;
	if (BeginAllocFixedInReservation(reservation.base, page_size, Mode::ReadWrite, &activate) != ReservedFixedResult::Begun) return false;
	activate.Commit();
	MutationTransaction second;
	if (BeginAllocFixedInReservation(reservation.base + page_size, page_size, Mode::ReadWrite, &second) != ReservedFixedResult::Begun) return false;
	second.Commit();
	baseline = GetMutationGeneration();
	if (!observe(reservation.base, bytes, baseline) ||
	    !Protect(reservation.base + page_size, page_size, Mode::Read) ||
	    !Protect(reservation.base + page_size, page_size, Mode::ReadWrite) ||
	    observe(reservation.base, bytes, baseline) || !observe(reservation.base, 8, baseline)) return false;
	baseline = GetMutationGeneration();
	FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest::DecommitHost);
	if (Free(reservation.base) || GetMutationGeneration() != baseline || observe(reservation.base, 8, baseline)) return false;
	if (!Free(reservation.base)) return false;
	MutationTransaction replacement;
	if (BeginAllocFixedInReservation(reservation.base, page_size, Mode::ReadWrite, &replacement) != ReservedFixedResult::Begun) return false;
	replacement.Commit();
	if (observe(reservation.base, 8, baseline) || !observe(reservation.base, 8, GetMutationGeneration())) return false;
#endif
	std::printf("virtual-memory guest-data-continuity=ok unrelated-maps=accepted same-map-protect/revert=reject patch/revert=reject "
	            "remap/decommit/reactivate=reject partial-and-unbounded-mutations=reject exact-thread-owned-managed-lease=required\n");
	return true;
}

bool TestMutationGeneration(uint64_t page_size)
{
	const uint64_t initial = VirtualMemory::GetMutationGeneration();
	if (VirtualMemory::Alloc(0, 0, VirtualMemory::Mode::ReadWrite) != 0 || VirtualMemory::GetMutationGeneration() != initial)
	{
		return false;
	}

	const uint64_t mapping          = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ReadWrite);
	const uint64_t after_allocation = VirtualMemory::GetMutationGeneration();
	if (mapping == 0 || !AdvancedOnce(initial, after_allocation) || !VirtualMemory::IsManagedAddress(mapping) ||
	    !VirtualMemory::IsManagedAddress(mapping + page_size - 1) || VirtualMemory::IsManagedAddress(mapping - 1) ||
	    VirtualMemory::IsManagedAddress(mapping + page_size) || VirtualMemory::IsManagedAddress(0) ||
	    VirtualMemory::IsManagedAddress(reinterpret_cast<uint64_t>(&TestMutationGeneration)))
	{
		if (mapping != 0) VirtualMemory::Free(mapping);
		return false;
	}

	const bool     protected_ok  = VirtualMemory::Protect(mapping, page_size, VirtualMemory::Mode::Read);
	const uint64_t after_protect = VirtualMemory::GetMutationGeneration();
	if (!protected_ok || !AdvancedOnce(after_allocation, after_protect) || !VirtualMemory::IsManagedAddress(mapping))
	{
		VirtualMemory::Free(mapping);
		return false;
	}

	const bool     freed_ok   = VirtualMemory::Free(mapping);
	const uint64_t after_free = VirtualMemory::GetMutationGeneration();
	if (!freed_ok || !AdvancedOnce(after_protect, after_free) || VirtualMemory::IsManagedAddress(mapping))
	{
		return false;
	}

	return !VirtualMemory::Free(mapping) && VirtualMemory::GetMutationGeneration() == after_free;
}

bool TestManagedAddressCrossThreadInvalidation(uint64_t page_size)
{
	const uint64_t mapping = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0) return false;

	std::atomic<uint32_t> stage {0};
	bool                  worker_ok = false;
	std::thread           worker(
        [&]
        {
            const bool initially_managed = VirtualMemory::IsManagedAddress(mapping);
            stage.store(1, std::memory_order_release);
            while (stage.load(std::memory_order_acquire) == 1)
                std::this_thread::yield();
            const bool released = !VirtualMemory::IsManagedAddress(mapping);
            stage.store(3, std::memory_order_release);
            while (stage.load(std::memory_order_acquire) == 3)
                std::this_thread::yield();
            worker_ok = initially_managed && released && VirtualMemory::IsManagedAddress(mapping);
        });
	while (stage.load(std::memory_order_acquire) == 0)
		std::this_thread::yield();
	const bool freed = VirtualMemory::Free(mapping);
	stage.store(2, std::memory_order_release);
	while (stage.load(std::memory_order_acquire) != 3)
		std::this_thread::yield();
	const bool remapped = freed && VirtualMemory::AllocFixed(mapping, page_size, VirtualMemory::Mode::ReadWrite);
	stage.store(4, std::memory_order_release);
	worker.join();
	const bool released = remapped ? VirtualMemory::Free(mapping) : (freed || VirtualMemory::Free(mapping));
	return freed && remapped && worker_ok && released;
}

bool TestAllocateProtectAndFree(uint64_t page_size)
{
	const uint64_t size    = 2 * page_size + 37;
	const uint64_t mapping = VirtualMemory::Alloc(0, size, VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0)
	{
		return false;
	}

	bool ok = HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE,
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	                        VM_PROT_EXECUTE
#else
	                        VM_PROT_NONE
#endif
	);
	auto* bytes     = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(mapping));
	bytes[0]        = 0x31;
	bytes[size - 1] = 0x7a;

	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	const bool strict = VirtualMemory::RequiresUniformHostPageDataProtection();
	ok                           = ok && VirtualMemory::Protect(mapping + (strict ? 0 : 32), strict ? page_size : 64,
	                                                           VirtualMemory::Mode::Read, &old_mode) &&
	     old_mode == VirtualMemory::Mode::ExecuteReadWrite &&
	     (strict || page_size == GUEST_PAGE_SIZE ? HasProtection(mapping, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE)
	                                   : HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE));

	old_mode = VirtualMemory::Mode::NoAccess;
	ok       = ok && VirtualMemory::Protect(mapping, page_size, VirtualMemory::Mode::ExecuteReadWrite, &old_mode) &&
	     old_mode == VirtualMemory::Mode::Read &&
	     HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE,
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	                   VM_PROT_EXECUTE
#else
	                   VM_PROT_NONE
#endif
	     );

	ok = ok && VirtualMemory::Free(mapping) && !VirtualMemory::Free(mapping);
	Region released {};
	ok = ok && !QueryRegion(mapping, &released);
	return ok;
}

bool TestUnalignedProtection(uint64_t page_size)
{
	const uint64_t visible_size = 3 * page_size + 17;
	const uint64_t mapping      = VirtualMemory::Alloc(0, visible_size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0)
	{
		return false;
	}

	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	const bool strict = VirtualMemory::RequiresUniformHostPageDataProtection();
	bool                ok       = VirtualMemory::Protect(mapping + (strict ? 1 : page_size - 8), strict ? 2 * page_size - 1 : 16,
	                                                       VirtualMemory::Mode::Read, &old_mode) &&
	          old_mode == VirtualMemory::Mode::ReadWrite &&
	          (strict || page_size == GUEST_PAGE_SIZE ? HasProtection(mapping, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE) &&
	                                              HasProtection(mapping + page_size, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE)
	                                        : HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) &&
	                                              HasProtection(mapping + page_size, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE)) &&
	          HasProtection(mapping + 2 * page_size, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE);

	old_mode           = VirtualMemory::Mode::Execute;
	const bool outside = VirtualMemory::Protect(mapping + visible_size - 8, 16, VirtualMemory::Mode::Read, &old_mode);
	ok                 = ok && !outside && old_mode == VirtualMemory::Mode::Execute;

	ok = VirtualMemory::Free(mapping) && ok;
	return ok;
}

bool TestGuestPageMetadata(uint64_t host_page_size)
{
	if (host_page_size < GUEST_PAGE_SIZE || host_page_size % GUEST_PAGE_SIZE != 0)
	{
		return false;
	}

	constexpr uint64_t alignment    = 64 * 1024;
	const bool strict = VirtualMemory::RequiresUniformHostPageDataProtection();
	const uint64_t granule = strict ? host_page_size : GUEST_PAGE_SIZE;
	const uint64_t visible_size = 6 * granule + 123;
	const uint64_t     mapping      = VirtualMemory::AllocAligned(0, visible_size, VirtualMemory::Mode::ReadWrite, alignment);
	if (mapping == 0)
	{
		return false;
	}

	const uint64_t read_write_patch = mapping + granule - sizeof(uint32_t);
	const uint64_t execute_patch    = mapping + 3 * granule - sizeof(uint32_t);
	uint64_t       first_seed       = 0x0123456789abcdefull;
	uint64_t       second_seed      = 0xfedcba9876543210ull;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(read_write_patch)), &first_seed, sizeof(first_seed));
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(execute_patch)), &second_seed, sizeof(second_seed));

	const VirtualMemory::Mode modes[4] = {VirtualMemory::Mode::Read, VirtualMemory::Mode::ReadWrite, VirtualMemory::Mode::NoAccess,
	                                      VirtualMemory::Mode::Execute};
	bool                      ok       = true;
	for (uint64_t page = 0; page < 4; page++)
	{
		VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
		ok                           = VirtualMemory::Protect(mapping + page * granule, granule, modes[page], &old_mode) &&
		     old_mode == VirtualMemory::Mode::ReadWrite && ok;
	}

	const auto visible_is = [](uint64_t address, uint64_t begin, uint64_t end, VirtualMemory::Mode mode)
	{
		VirtualMemory::ManagedRegion region {};
		return VirtualMemory::QueryManagedRegion(address, &region) == VirtualMemory::ManagedRegionState::Visible && region.begin == begin &&
		       region.end == end && region.mode == mode;
	};
	ok = ok && visible_is(mapping + 17, mapping, mapping + granule, VirtualMemory::Mode::Read) &&
	     visible_is(mapping + granule + 17, mapping + granule, mapping + 2 * granule,
	                VirtualMemory::Mode::ReadWrite) &&
	     visible_is(mapping + 2 * granule + 17, mapping + 2 * granule, mapping + 3 * granule,
	                VirtualMemory::Mode::NoAccess) &&
	     visible_is(mapping + 3 * granule + 17, mapping + 3 * granule, mapping + 4 * granule,
	                VirtualMemory::Mode::Execute) &&
	     visible_is(mapping + visible_size - 1, mapping + 4 * granule, mapping + visible_size, VirtualMemory::Mode::ReadWrite);

	VirtualMemory::ManagedRegion padding {};
	const uint64_t               padding_probe = mapping + visible_size;
	ok = ok && VirtualMemory::QueryManagedRegion(padding_probe, &padding) == VirtualMemory::ManagedRegionState::Reserved &&
	     padding.begin == padding_probe && padding.end > padding.begin && padding.mode == VirtualMemory::Mode::NoAccess &&
	     !VirtualMemory::IsManagedAddress(padding_probe) && VirtualMemory::IsManagedAddress(mapping + visible_size - 1);

	VirtualMemory::ManagedRegion unmanaged {1, 2, VirtualMemory::Mode::Read};
	ok = ok &&
	     VirtualMemory::QueryManagedRegion(reinterpret_cast<uint64_t>(&TestGuestPageMetadata), &unmanaged) ==
	         VirtualMemory::ManagedRegionState::Unmanaged &&
	     unmanaged.begin == 0 && unmanaged.end == 0 && unmanaged.mode == VirtualMemory::Mode::NoAccess &&
	     VirtualMemory::QueryManagedRegion(0, nullptr) == VirtualMemory::ManagedRegionState::Unmanaged;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	VirtualMemory::ManagedRegion snapshot_region {1, 2, VirtualMemory::Mode::Read};
	auto snapshot_state = VirtualMemory::ManagedRegionState::Unmanaged;
	const bool snapshot_available = VirtualMemory::TryQueryManagedRegionSnapshot(
	    mapping + granule + 17, &snapshot_state, &snapshot_region);
	if (VirtualMemory::IsImmutableRegionSnapshotEnabled())
	{
		ok = ok && snapshot_available &&
		     snapshot_state == VirtualMemory::ManagedRegionState::Visible &&
		     snapshot_region.begin == mapping + granule &&
		     snapshot_region.end == mapping + 2 * granule &&
		     snapshot_region.mode == VirtualMemory::Mode::ReadWrite;
	} else
	{
		ok = ok && !snapshot_available &&
		     snapshot_state == VirtualMemory::ManagedRegionState::Unmanaged &&
		     snapshot_region.begin == 0 && snapshot_region.end == 0 &&
		     snapshot_region.mode == VirtualMemory::Mode::NoAccess;
	}
#endif

	if (!strict && host_page_size >= 4 * granule)
	{
		ok = ok && HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE);
	} else
	{
		ok = ok && HasProtection(mapping, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE) &&
		     HasProtection(mapping + granule, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) &&
		     HasProtection(mapping + 2 * granule, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
		ok = ok && HasProtection(mapping + 3 * granule, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE);
#else
		ok = ok && HasProtection(mapping + 3 * granule, VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_WRITE);
#endif
	}

	const uint64_t      before_same_envelope  = VirtualMemory::GetMutationGeneration();
	VirtualMemory::Mode old_mode              = VirtualMemory::Mode::NoAccess;
	const bool          changed_same_envelope = VirtualMemory::Protect(mapping + 32, granule - 32, VirtualMemory::Mode::ExecuteRead, &old_mode);
	const uint64_t      after_same_envelope   = VirtualMemory::GetMutationGeneration();
	ok = ok && changed_same_envelope && old_mode == VirtualMemory::Mode::Read && AdvancedOnce(before_same_envelope, after_same_envelope) &&
	     visible_is(mapping, mapping, mapping + granule, VirtualMemory::Mode::ExecuteRead) &&
	     VirtualMemory::Protect(mapping, granule, VirtualMemory::Mode::Read, &old_mode) && old_mode == VirtualMemory::Mode::ExecuteRead;

	old_mode = VirtualMemory::Mode::NoAccess;
	ok       = ok && VirtualMemory::Protect(mapping, 2 * granule, VirtualMemory::Mode::Read, &old_mode) &&
	     old_mode == VirtualMemory::Mode::Read &&
	     visible_is(mapping + granule, mapping, mapping + 2 * granule, VirtualMemory::Mode::Read) &&
	     VirtualMemory::Protect(mapping + granule, granule, VirtualMemory::Mode::ReadWrite, &old_mode) &&
	     old_mode == VirtualMemory::Mode::Read;

	const uint64_t first_replacement  = 0x8877665544332211ull;
	const uint64_t second_replacement = 0x1020304050607080ull;
	ok                                = ok && VirtualMemory::PatchReplace(read_write_patch, first_replacement);
	std::memcpy(&first_seed, reinterpret_cast<void*>(static_cast<uintptr_t>(read_write_patch)), sizeof(first_seed));
	ok = ok && first_seed == first_replacement && !VirtualMemory::PatchReplace(read_write_patch, first_replacement) &&
	     visible_is(mapping, mapping, mapping + granule, VirtualMemory::Mode::Read) &&
	     visible_is(mapping + granule, mapping + granule, mapping + 2 * granule, VirtualMemory::Mode::ReadWrite) &&
	     VirtualMemory::PatchReplace(execute_patch, second_replacement) &&
	     !VirtualMemory::PatchReplace(execute_patch, second_replacement) &&
	     visible_is(mapping + 2 * granule, mapping + 2 * granule, mapping + 3 * granule,
	                VirtualMemory::Mode::NoAccess) &&
	     visible_is(mapping + 3 * granule, mapping + 3 * granule, mapping + 4 * granule,
	                VirtualMemory::Mode::Execute);

	VirtualMemory::ManagedRegion tail_before {};
	VirtualMemory::ManagedRegion tail_after {};
	const auto                   tail_state     = VirtualMemory::QueryManagedRegion(mapping + visible_size - 1, &tail_before);
	const uint64_t               before_failure = VirtualMemory::GetMutationGeneration();
	old_mode                                    = VirtualMemory::Mode::ExecuteWrite;
	const bool     failed_protect = VirtualMemory::Protect(mapping + visible_size - 4, 8, VirtualMemory::Mode::Read, &old_mode);
	const uint64_t after_failure  = VirtualMemory::GetMutationGeneration();
	ok                            = ok && tail_state == VirtualMemory::ManagedRegionState::Visible && !failed_protect &&
	     old_mode == VirtualMemory::Mode::ExecuteWrite && before_failure == after_failure &&
	     VirtualMemory::QueryManagedRegion(mapping + visible_size - 1, &tail_after) == VirtualMemory::ManagedRegionState::Visible &&
	     tail_after.begin == tail_before.begin && tail_after.end == tail_before.end && tail_after.mode == tail_before.mode &&
	     HasProtection(mapping + 4 * granule, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE);

	const uint64_t               padding_end = padding.end;
	const bool                   freed       = VirtualMemory::Free(mapping);
	VirtualMemory::ManagedRegion released {};
	return ok && freed && VirtualMemory::QueryManagedRegion(mapping, &released) == VirtualMemory::ManagedRegionState::Unmanaged &&
	       VirtualMemory::QueryManagedRegion(padding_end - 1, &released) == VirtualMemory::ManagedRegionState::Unmanaged;
}

bool TestAlignedAndFixed(uint64_t page_size)
{
	constexpr uint64_t alignment = 64 * 1024;
	const uint64_t     size      = 2 * page_size + 31;
	const uint64_t     mapping   = VirtualMemory::AllocAligned(0, size, VirtualMemory::Mode::ReadWrite, alignment);
	if (mapping == 0 || (mapping & (alignment - 1)) != 0)
	{
		if (mapping != 0) VirtualMemory::Free(mapping);
		return false;
	}

	auto* bytes                  = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(mapping));
	bytes[0]                     = 0x5c;
	const uint64_t padding_probe = (mapping + size + page_size - 1) & ~(page_size - 1);
	Region         padding_region {};
	const bool     raw_padding_mapped = QueryRegion(padding_probe, &padding_region);
	const bool     occupied_rejected  = !VirtualMemory::AllocFixed(mapping, size, VirtualMemory::Mode::ReadWrite) && bytes[0] == 0x5c;
	if (!VirtualMemory::Free(mapping))
	{
		return false;
	}

	Region     released_padding {};
	const bool raw_padding_released = !QueryRegion(padding_probe, &released_padding);
	const bool fixed                = VirtualMemory::AllocFixed(mapping, size, VirtualMemory::Mode::ReadWrite);
	const bool ok                   = raw_padding_mapped && raw_padding_released && occupied_rejected && fixed &&
	                HasProtection(mapping, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE);
	return fixed ? (VirtualMemory::Free(mapping) && ok) : false;
}

bool TestPatchReplace(uint64_t page_size)
{
	const uint64_t mapping = VirtualMemory::Alloc(0, 2 * page_size, VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0)
	{
		return false;
	}

	const uint64_t patch_address = mapping + page_size - sizeof(uint32_t);
	uint64_t       stored_value  = 0x1122334455667788ull;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(patch_address)), &stored_value, sizeof(stored_value));

	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	bool                ok       = VirtualMemory::Protect(mapping, page_size, VirtualMemory::Mode::ExecuteRead, &old_mode) &&
	          old_mode == VirtualMemory::Mode::ExecuteReadWrite &&
	          VirtualMemory::Protect(mapping + page_size, page_size, VirtualMemory::Mode::Read, &old_mode) &&
	          old_mode == VirtualMemory::Mode::ExecuteReadWrite;
	ok = ok && VirtualMemory::PatchReplace(patch_address, 0x8877665544332211ull);
	std::memcpy(&stored_value, reinterpret_cast<void*>(static_cast<uintptr_t>(patch_address)), sizeof(stored_value));
	ok = ok && stored_value == 0x8877665544332211ull;
	ok = ok && !VirtualMemory::PatchReplace(patch_address, 0x8877665544332211ull);
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	ok = ok && HasProtection(mapping, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE);
	ok = ok && HasProtection(mapping + page_size, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE);
#else
	ok = ok && HasProtection(mapping, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_WRITE);
	ok = ok && HasProtection(mapping + page_size, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE);
#endif
	ok = ok && VirtualMemory::Protect(mapping, 1, VirtualMemory::Mode::ExecuteRead, &old_mode) &&
	     old_mode == VirtualMemory::Mode::ExecuteRead;
	ok =
	    ok && VirtualMemory::Protect(mapping + page_size, 1, VirtualMemory::Mode::Read, &old_mode) && old_mode == VirtualMemory::Mode::Read;
	ok = VirtualMemory::Free(mapping) && ok;
	return ok;
}

bool TestConcurrentPatch(uint64_t page_size)
{
	constexpr uint32_t thread_count = 4;
	constexpr uint32_t iterations   = 128;
	const uint64_t     mapping      = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0)
	{
		return false;
	}

	const uint64_t patch_address = mapping + 1;
	uint64_t       initial_value = 0;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(patch_address)), &initial_value, sizeof(initial_value));
	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	if (!VirtualMemory::Protect(mapping, page_size, VirtualMemory::Mode::ExecuteRead, &old_mode))
	{
		VirtualMemory::Free(mapping);
		return false;
	}

	std::atomic_bool     start {false};
	std::atomic_bool     stop {false};
	std::atomic_bool     ok {true};
	std::atomic_bool     reader_ready {false};
	std::atomic_uint64_t query_count {0};
	std::thread          query_reader(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            const uint64_t probe = mapping + page_size / 2;
            while (!stop.load(std::memory_order_acquire) && ok.load(std::memory_order_relaxed))
            {
                VirtualMemory::ManagedRegion region {};
                if (VirtualMemory::QueryManagedRegion(probe, &region) != VirtualMemory::ManagedRegionState::Visible ||
                    region.mode != VirtualMemory::Mode::ExecuteRead || region.begin > probe || region.end <= probe)
                {
                    ok.store(false, std::memory_order_relaxed);
                    break;
                }
                query_count.fetch_add(1, std::memory_order_relaxed);
                reader_ready.store(true, std::memory_order_release);
            }
        });
	std::thread workers[thread_count];
	for (uint32_t thread_index = 0; thread_index < thread_count; thread_index++)
	{
		workers[thread_index] = std::thread(
		    [&, thread_index]
		    {
			    while (!start.load(std::memory_order_acquire))
			    {
				    std::this_thread::yield();
			    }
			    while (!reader_ready.load(std::memory_order_acquire) && ok.load(std::memory_order_relaxed))
			    {
				    std::this_thread::yield();
			    }
			    for (uint32_t iteration = 0; iteration < iterations && ok.load(std::memory_order_relaxed); iteration++)
			    {
				    const uint64_t value = 0xa500000000000000ull | (static_cast<uint64_t>(thread_index) << 32u) | (iteration + 1u);
				    if (!VirtualMemory::PatchReplace(patch_address, value))
				    {
					    ok.store(false, std::memory_order_relaxed);
					    break;
				    }
			    }
		    });
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers)
	{
		worker.join();
	}
	stop.store(true, std::memory_order_release);
	query_reader.join();

	uint64_t final_value = 0;
	std::memcpy(&final_value, reinterpret_cast<void*>(static_cast<uintptr_t>(patch_address)), sizeof(final_value));
	const bool value_valid = (final_value & 0xff00000000000000ull) == 0xa500000000000000ull;
	bool       result      = ok.load(std::memory_order_relaxed) && value_valid && query_count.load(std::memory_order_relaxed) != 0;
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	result = result && HasProtection(mapping, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE);
#else
	result = result && HasProtection(mapping, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_WRITE);
#endif
	result = result && VirtualMemory::Protect(mapping, 1, VirtualMemory::Mode::ExecuteRead, &old_mode) &&
	         old_mode == VirtualMemory::Mode::ExecuteRead;
	result = VirtualMemory::Free(mapping) && result;
	return result;
}

bool TestConcurrentLifecycle(uint64_t page_size)
{
	const uint64_t protection_size = VirtualMemory::RequiresUniformHostPageDataProtection() ? page_size : 1;
	constexpr uint32_t thread_count = 4;
	constexpr uint32_t iterations   = 64;
	std::atomic_bool   ok {true};
	std::thread        workers[thread_count];

	for (auto& worker: workers)
	{
		worker = std::thread(
		    [&]
		    {
			    for (uint32_t iteration = 0; iteration < iterations && ok.load(std::memory_order_relaxed); iteration++)
			    {
				    const uint64_t      mapping = VirtualMemory::AllocAligned(0, page_size + 17, VirtualMemory::Mode::ReadWrite, 64 * 1024);
				    VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
				    if (mapping == 0 || !VirtualMemory::Protect(mapping, protection_size, VirtualMemory::Mode::Read, &old_mode) ||
				        old_mode != VirtualMemory::Mode::ReadWrite ||
				        !VirtualMemory::Protect(mapping, protection_size, VirtualMemory::Mode::ReadWrite, &old_mode) ||
				        old_mode != VirtualMemory::Mode::Read)
				    {
					    if (mapping != 0) VirtualMemory::Free(mapping);
					    ok.store(false, std::memory_order_relaxed);
					    break;
				    }
				    if (!VirtualMemory::Free(mapping))
				    {
					    ok.store(false, std::memory_order_relaxed);
					    break;
				    }
			    }
		    });
	}
	for (auto& worker: workers)
	{
		worker.join();
	}
	return ok.load(std::memory_order_relaxed);
}

bool TestImmutableSnapshotDifferential()
{
	constexpr uint64_t alignment    = 64 * 1024;
	const uint64_t granule = VirtualMemory::RequiresUniformHostPageDataProtection()
	                             ? static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) : GUEST_PAGE_SIZE;
	const uint64_t visible_size = 17 * granule + 123;
	constexpr size_t   page_count   = 18;
	const uint64_t     mapping      = VirtualMemory::AllocAligned(0, visible_size, VirtualMemory::Mode::ReadWrite, alignment);
	if (mapping == 0) return false;

	VirtualMemory::Mode expected[page_count];
	for (auto& mode: expected)
		mode = VirtualMemory::Mode::ReadWrite;
	const VirtualMemory::Mode modes[] = {VirtualMemory::Mode::NoAccess, VirtualMemory::Mode::Read, VirtualMemory::Mode::ReadWrite,
	                                     VirtualMemory::Mode::ExecuteRead};
	uint64_t                  random  = 0x9e3779b97f4a7c15ull;
	bool                      ok      = true;
	for (uint32_t iteration = 0; iteration < 384 && ok; iteration++)
	{
		random ^= random << 7u;
		random ^= random >> 9u;
		random ^= random << 8u;
		const size_t   first         = static_cast<size_t>(random % page_count);
		const size_t   remaining     = page_count - first;
		const size_t   count         = 1 + static_cast<size_t>((random >> 8u) % remaining);
		const auto     mode          = modes[(random >> 16u) % (sizeof(modes) / sizeof(modes[0]))];
		const uint64_t offset        = VirtualMemory::RequiresUniformHostPageDataProtection() ? 0 : (random >> 24u) & 63u;
		const uint64_t address       = mapping + first * granule + offset;
		const uint64_t requested_end = mapping + (first + count) * granule;
		const uint64_t clipped_end   = requested_end < mapping + visible_size ? requested_end : mapping + visible_size;
		if (address >= clipped_end || !VirtualMemory::Protect(address, clipped_end - address, mode))
		{
			ok = false;
			break;
		}
		for (size_t page = first; page < first + count; page++)
			expected[page] = mode;

		for (size_t page = 0; page < page_count; page++)
		{
			const uint64_t page_begin  = mapping + page * granule;
			const uint64_t probe       = page + 1 == page_count ? mapping + visible_size - 1 : page_begin + 37;
			size_t         first_equal = page;
			while (first_equal != 0 && expected[first_equal - 1] == expected[page])
				first_equal--;
			size_t last_equal = page + 1;
			while (last_equal < page_count && expected[last_equal] == expected[page])
				last_equal++;
			const uint64_t expected_begin   = mapping + first_equal * granule;
			const uint64_t raw_expected_end = mapping + last_equal * granule;
			const uint64_t expected_end     = raw_expected_end < mapping + visible_size ? raw_expected_end : mapping + visible_size;
			VirtualMemory::ManagedRegion region {};
			if (VirtualMemory::QueryManagedRegion(probe, &region) != VirtualMemory::ManagedRegionState::Visible ||
			    region.begin != expected_begin || region.end != expected_end || region.mode != expected[page])
			{
				ok = false;
				break;
			}
		}
	}

	VirtualMemory::ManagedRegion padding {};
	const bool                   reserved =
	    VirtualMemory::QueryManagedRegion(mapping + visible_size, &padding) == VirtualMemory::ManagedRegionState::Reserved &&
	    padding.begin == mapping + visible_size && padding.end > padding.begin && padding.mode == VirtualMemory::Mode::NoAccess;
	const bool                   freed = VirtualMemory::Free(mapping);
	VirtualMemory::ManagedRegion released {};
	return ok && reserved && freed && VirtualMemory::QueryManagedRegion(mapping, &released) == VirtualMemory::ManagedRegionState::Unmanaged;
}

bool TestImmutableSnapshotConcurrentReaders()
{
	constexpr uint64_t size         = 64 * GUEST_PAGE_SIZE;
	constexpr uint32_t reader_count = 6;
	constexpr uint32_t iterations   = 512;
	const uint64_t     mapping      = VirtualMemory::Alloc(0, size, VirtualMemory::Mode::ReadWrite);
	if (mapping == 0) return false;

	std::atomic_bool start {false};
	std::atomic_bool stop {false};
	std::atomic_bool ok {true};
	std::thread      readers[reader_count];
	for (uint32_t index = 0; index < reader_count; index++)
	{
		readers[index] = std::thread(
		    [&, index]
		    {
			    while (!start.load(std::memory_order_acquire))
				    std::this_thread::yield();
			    const uint64_t probe = mapping + ((static_cast<uint64_t>(index) * 11u) % 64u) * GUEST_PAGE_SIZE + 19u;
			    while (!stop.load(std::memory_order_acquire) && ok.load(std::memory_order_relaxed))
			    {
				    VirtualMemory::ManagedRegion region {};
				    const auto                   state = VirtualMemory::QueryManagedRegion(probe, &region);
				    const bool mode_ok = region.mode == VirtualMemory::Mode::Read || region.mode == VirtualMemory::Mode::ReadWrite;
				    if (state != VirtualMemory::ManagedRegionState::Visible || !mode_ok || region.begin < mapping ||
				        region.end > mapping + size || region.begin > probe || region.end <= probe)
				    {
					    ok.store(false, std::memory_order_relaxed);
				    }
			    }
		    });
	}

	start.store(true, std::memory_order_release);
	for (uint32_t iteration = 0; iteration < iterations && ok.load(std::memory_order_relaxed); iteration++)
	{
		const auto mode = (iteration & 1u) == 0 ? VirtualMemory::Mode::Read : VirtualMemory::Mode::ReadWrite;
		if (!VirtualMemory::Protect(mapping, size, mode))
		{
			ok.store(false, std::memory_order_relaxed);
		}
	}
	stop.store(true, std::memory_order_release);
	for (auto& reader: readers)
		reader.join();
	const bool freed = VirtualMemory::Free(mapping);
	return ok.load(std::memory_order_relaxed) && freed;
}

bool TestImmutableSnapshotBoundedWindow()
{
	constexpr uint64_t           owner_begin = 0x1003;
	constexpr uint64_t           owner_end   = owner_begin + 1024 * GUEST_PAGE_SIZE;
	constexpr uint64_t           target_page = 400;
	const uint64_t               address     = target_page * GUEST_PAGE_SIZE + 71;
	VirtualMemory::ManagedRegion middle {owner_begin, owner_end, VirtualMemory::Mode::ReadWrite};
	const bool middle_ok = VirtualMemory::ImmutableSnapshot::ClipRegionToPageWindow(address, GUEST_PAGE_SIZE, 127, 128, &middle) &&
	                       middle.begin == (target_page - 127) * GUEST_PAGE_SIZE && middle.end == (target_page + 129) * GUEST_PAGE_SIZE;

	VirtualMemory::ManagedRegion leading {owner_begin, owner_end, VirtualMemory::Mode::ReadWrite};
	const uint64_t               leading_address = 3 * GUEST_PAGE_SIZE + 9;
	const bool                   leading_ok =
	    VirtualMemory::ImmutableSnapshot::ClipRegionToPageWindow(leading_address, GUEST_PAGE_SIZE, 127, 128, &leading) &&
	    leading.begin == owner_begin && leading.end == 132 * GUEST_PAGE_SIZE;

	VirtualMemory::ManagedRegion trailing {owner_begin, owner_end, VirtualMemory::Mode::ReadWrite};
	const uint64_t               trailing_address = 1000 * GUEST_PAGE_SIZE + 5;
	const bool                   trailing_ok =
	    VirtualMemory::ImmutableSnapshot::ClipRegionToPageWindow(trailing_address, GUEST_PAGE_SIZE, 127, 128, &trailing) &&
	    trailing.begin == 873 * GUEST_PAGE_SIZE && trailing.end == owner_end;
	return middle_ok && leading_ok && trailing_ok;
}

bool TestImmutableSnapshotExecutionPinnedLookup()
{
	// Model an additive publication while an independent FEX execution barrier
	// pins the existing mapping. The special reader may inspect the prior or
	// new snapshot; ordinary readers must still reject the mutation interval.
	VirtualMemory::ImmutableSnapshot::Publisher publisher;
	VirtualMemory::ImmutableSnapshot::Builder initial;
	if (!initial.Append({0x1000, 0x2000, 1, VirtualMemory::ManagedRegionState::Visible, VirtualMemory::Mode::ReadWrite}) ||
	    !publisher.Publish(initial)) return false;
	publisher.BeginMutation();
	VirtualMemory::ManagedRegion region {};
	auto state = VirtualMemory::ManagedRegionState::Unmanaged;
	bool ok = !publisher.Lookup(0x1008, &state, &region, false) &&
	          publisher.Lookup(0x1008, &state, &region, false, true) &&
	          state == VirtualMemory::ManagedRegionState::Visible && region.mode == VirtualMemory::Mode::ReadWrite;
	VirtualMemory::ImmutableSnapshot::Builder additive;
	ok = additive.Append({0x1000, 0x2000, 1, VirtualMemory::ManagedRegionState::Visible, VirtualMemory::Mode::ReadWrite}) &&
	     additive.Append({0x3000, 0x4000, 2, VirtualMemory::ManagedRegionState::Visible, VirtualMemory::Mode::Read}) &&
	     publisher.Publish(additive) &&
	     !publisher.Lookup(0x1008, &state, &region, false) &&
	     publisher.Lookup(0x1008, &state, &region, false, true) && region.mode == VirtualMemory::Mode::ReadWrite &&
	     publisher.Lookup(0x3008, &state, &region, false, true) && region.mode == VirtualMemory::Mode::Read && ok;
	publisher.EndMutation();
	return publisher.Lookup(0x1008, &state, &region, false) && region.mode == VirtualMemory::Mode::ReadWrite && ok;
}

bool WaitForStarted(const std::atomic_bool& started)
{
	for (uint32_t attempt = 0; attempt < 1000000; attempt++)
	{
		if (started.load(std::memory_order_acquire)) return true;
		std::this_thread::yield();
	}
	return false;
}

bool ObservePendingMutationReject(uint64_t address)
{
	for (uint32_t attempt = 0; attempt < 1000000; attempt++)
	{
		VirtualMemory::CodeReadLease probe;
		if (!VirtualMemory::TryAcquireCodeReadLease(address, 1, &probe)) return true;
		VirtualMemory::ReleaseCodeReadLease(&probe);
		std::this_thread::yield();
	}
	return false;
}

bool MeasureCodeReadLeaseHotPath(uint64_t address)
{
	VirtualMemory::CodeReadLease warm;
	if (!VirtualMemory::TryAcquireCodeReadLease(address, 8, &warm)) return false;
	VirtualMemory::ReleaseCodeReadLease(&warm);

	constexpr uint32_t iterations = 200000;
	const auto         begin      = std::chrono::steady_clock::now();
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		VirtualMemory::CodeReadLease lease;
		if (!VirtualMemory::TryAcquireCodeReadLease(address, 8, &lease)) return false;
		VirtualMemory::ReleaseCodeReadLease(&lease);
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
	std::printf("code-read-lease-hot-path=%.2f ns/acquire-release (%u iterations)\n", static_cast<double>(elapsed) / iterations,
	            iterations);
	return true;
}

bool TestCodeReadLease(uint64_t host_page_size)
{
	static_assert(!std::is_copy_constructible_v<VirtualMemory::CodeReadLease>);
	static_assert(!std::is_copy_assignable_v<VirtualMemory::CodeReadLease>);

	VirtualMemory::CodeReadLease invalid;
	bool ok = !VirtualMemory::TryAcquireCodeReadLease(reinterpret_cast<uint64_t>(&TestCodeReadLease), 1, &invalid) && !invalid.IsActive() &&
	          !VirtualMemory::TryAcquireCodeReadLease(0, 1, &invalid) &&
	          !VirtualMemory::TryAcquireCodeReadLease((std::numeric_limits<uint64_t>::max)() - 3, 8, &invalid);
	VirtualMemory::ReleaseCodeReadLease(&invalid);

	const uint64_t size    = 2 * host_page_size;
	const uint64_t mapping = VirtualMemory::Alloc(0, size, VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0) return false;
	uint64_t seed = UINT64_C(0x1122334455667788);
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping + 64)), &seed, sizeof(seed));

	VirtualMemory::CodeReadLease writable;
	ok = ok && !VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &writable) && !writable.IsActive();

	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	if (host_page_size > GUEST_PAGE_SIZE)
	{
		const bool strict = VirtualMemory::RequiresUniformHostPageDataProtection();
		ok = ok && VirtualMemory::Protect(mapping, GUEST_PAGE_SIZE, VirtualMemory::Mode::ExecuteRead, &old_mode) == !strict &&
		     old_mode == (strict ? VirtualMemory::Mode::NoAccess : VirtualMemory::Mode::ExecuteReadWrite);
		VirtualMemory::CodeReadLease sibling_writable;
		ok = ok && !VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &sibling_writable);
	}
	ok = ok && VirtualMemory::Protect(mapping, size, VirtualMemory::Mode::ExecuteRead, &old_mode);

	VirtualMemory::CodeReadLease cross_page;
	ok = ok && !VirtualMemory::TryAcquireCodeReadLease(mapping + GUEST_PAGE_SIZE - 4, 8, &cross_page);

	// Logical metadata alone is insufficient: prove that an out-of-band physical
	// permission change is observed by the Mach query before granting a lease.
	const bool physical_writable = mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)), static_cast<size_t>(host_page_size),
	                                        PROT_READ | PROT_WRITE) == 0;
	VirtualMemory::CodeReadLease physical_mismatch;
	ok = ok && physical_writable && !VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &physical_mismatch) &&
	     mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)), static_cast<size_t>(host_page_size), PROT_READ) == 0;
	ok = ok && MeasureCodeReadLeaseHotPath(mapping + 64);

	VirtualMemory::CodeReadLease first;
	VirtualMemory::CodeReadLease second;
	ok = ok && VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &first) &&
	     VirtualMemory::TryAcquireCodeReadLease(mapping + 128, sizeof(seed), &second) && first.IsActive() &&
	     first.MappingIdentity() == second.MappingIdentity() && first.HostBegin() == mapping + 64 && first.ByteCount() == sizeof(seed);
	const uint64_t original_identity = first.MappingIdentity();
	ok = ok && !VirtualMemory::TryAcquireCodeReadLease(mapping + 192, sizeof(seed), &first) && first.IsActive() &&
	     first.MappingIdentity() == original_identity;

	std::atomic_bool protect_started {false};
	std::atomic_bool protect_done {false};
	bool             protect_result = false;
	std::thread      protect_worker(
        [&]
        {
            protect_started.store(true, std::memory_order_release);
            protect_result = VirtualMemory::Protect(mapping, size, VirtualMemory::Mode::ExecuteReadWrite);
            protect_done.store(true, std::memory_order_release);
        });
	const bool protect_pending = WaitForStarted(protect_started) && ObservePendingMutationReject(mapping + 64);
	VirtualMemory::ReleaseCodeReadLease(&first);
	const bool still_blocked = !protect_done.load(std::memory_order_acquire);
	VirtualMemory::ReleaseCodeReadLease(&second);
	protect_worker.join();
	ok = ok && protect_pending && still_blocked && protect_result && protect_done.load(std::memory_order_acquire);

	VirtualMemory::CodeReadLease now_writable;
	ok = ok && !VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &now_writable) &&
	     VirtualMemory::Protect(mapping, size, VirtualMemory::Mode::ExecuteRead, &old_mode);

	VirtualMemory::CodeReadLease patch_lease;
	ok                            = ok && VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &patch_lease);
	const uint64_t   before_patch = VirtualMemory::GetMutationGeneration();
	std::atomic_bool patch_started {false};
	bool             patch_result = false;
	std::thread      patch_worker(
        [&]
        {
            patch_started.store(true, std::memory_order_release);
            patch_result = VirtualMemory::PatchReplace(mapping + 64, UINT64_C(0x8877665544332211));
        });
	const bool patch_pending = WaitForStarted(patch_started) && ObservePendingMutationReject(mapping + 64);
	VirtualMemory::ReleaseCodeReadLease(&patch_lease);
	patch_worker.join();
	uint64_t patched = 0;
	std::memcpy(&patched, reinterpret_cast<const void*>(static_cast<uintptr_t>(mapping + 64)), sizeof(patched));
	ok = ok && patch_pending && patch_result && patched == UINT64_C(0x8877665544332211) &&
	     AdvancedOnce(before_patch, VirtualMemory::GetMutationGeneration());

	VirtualMemory::CodeReadLease free_lease;
	ok = ok && VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &free_lease);
	std::atomic_bool free_started {false};
	bool             free_result = false;
	std::thread      free_worker(
        [&]
        {
            free_started.store(true, std::memory_order_release);
            free_result = VirtualMemory::Free(mapping);
        });
	const bool free_pending = WaitForStarted(free_started) && ObservePendingMutationReject(mapping + 64);
	VirtualMemory::ReleaseCodeReadLease(&free_lease);
	free_worker.join();
	VirtualMemory::ReleaseCodeReadLease(&free_lease);
	VirtualMemory::CodeReadLease released;
	ok = ok && free_pending && free_result && !VirtualMemory::TryAcquireCodeReadLease(mapping + 64, 1, &released) &&
	     !VirtualMemory::IsManagedAddress(mapping) && VirtualMemory::AllocFixed(mapping, size, VirtualMemory::Mode::ExecuteRead);
	VirtualMemory::CodeReadLease remapped;
	ok = ok && VirtualMemory::TryAcquireCodeReadLease(mapping + 64, sizeof(seed), &remapped) && remapped.IsActive() &&
	     remapped.MappingIdentity() != original_identity;
	VirtualMemory::ReleaseCodeReadLease(&remapped);
	return VirtualMemory::Free(mapping) && ok;
}

bool TestSparseReservation(uint64_t page_size)
{
	const uint64_t                     alignment         = 4 * page_size;
	const uint64_t                     size              = 8 * page_size;
	const uint64_t                     generation_before = VirtualMemory::GetMutationGeneration();
	VirtualMemory::ReservationHandle   reservation {};
	VirtualMemory::MutationTransaction create;
	if (!VirtualMemory::BeginReserveAligned(0, size, alignment, &reservation, &create) || !create.IsActive() || !reservation.IsValid() ||
	    reservation.base % alignment != 0 || reservation.size != size ||
	    !AdvancedOnce(generation_before, VirtualMemory::GetMutationGeneration()))
	{
		(void)create.Abort();
		return false;
	}

	VirtualMemory::ManagedRegion region {};
	if (VirtualMemory::QueryManagedRegion(reservation.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    region.begin != reservation.base || region.end != reservation.base + reservation.size ||
	    VirtualMemory::IsManagedAddress(reservation.base) ||
	    !HasProtection(reservation.base, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE))
	{
		(void)create.Abort();
		return false;
	}
	create.Commit();
	if (create.IsActive()) return false;
	if (reservation.base < page_size || !VirtualMemory::HasReservationOverlap(reservation.base, page_size) ||
	    !VirtualMemory::HasReservationOverlap(reservation.base - page_size, 2 * page_size) ||
	    VirtualMemory::HasReservationOverlap(reservation.base + reservation.size, page_size))
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}

	const uint64_t                     aborted_base     = reservation.base + 2 * page_size;
	const uint64_t                     abort_generation = VirtualMemory::GetMutationGeneration();
	VirtualMemory::MutationTransaction aborted;
	if (VirtualMemory::BeginAllocFixedInReservation(aborted_base, page_size, VirtualMemory::Mode::ReadWrite, &aborted) !=
	        VirtualMemory::ReservedFixedResult::Begun ||
	    !aborted.IsActive() || !aborted.Abort() || aborted.IsActive() || VirtualMemory::GetMutationGeneration() != abort_generation + 2 ||
	    VirtualMemory::QueryManagedRegion(aborted_base, &region) != VirtualMemory::ManagedRegionState::Reserved)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}

	const uint64_t abandoned_base       = reservation.base + 3 * page_size;
	const uint64_t abandoned_generation = VirtualMemory::GetMutationGeneration();
	{
		VirtualMemory::MutationTransaction abandoned;
		if (VirtualMemory::BeginAllocFixedInReservation(abandoned_base, page_size, VirtualMemory::Mode::ReadWrite, &abandoned) !=
		    VirtualMemory::ReservedFixedResult::Begun)
		{
			(void)VirtualMemory::ReleaseReservation(reservation);
			return false;
		}
	}
	if (VirtualMemory::GetMutationGeneration() != abandoned_generation + 2 ||
	    VirtualMemory::QueryManagedRegion(abandoned_base, &region) != VirtualMemory::ManagedRegionState::Reserved)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}

	const uint64_t                     first   = reservation.base + page_size;
	const uint64_t                     sibling = reservation.base + 4 * page_size;
	VirtualMemory::MutationTransaction first_transaction;
	VirtualMemory::MutationTransaction sibling_transaction;
	if (VirtualMemory::BeginAllocFixedInReservation(first, page_size, VirtualMemory::Mode::ReadWrite, &first_transaction) !=
	    VirtualMemory::ReservedFixedResult::Begun)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}
	first_transaction.Commit();
	if (VirtualMemory::BeginAllocFixedInReservation(sibling, 2 * page_size, VirtualMemory::Mode::ReadWrite, &sibling_transaction) !=
	    VirtualMemory::ReservedFixedResult::Begun)
	{
		(void)sibling_transaction.Abort();
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}
	sibling_transaction.Commit();

	auto* first_bytes   = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(first));
	auto* sibling_bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(sibling));
	first_bytes[0]      = 0x5a;
	sibling_bytes[0]    = 0xa5;

	VirtualMemory::Mode          old_mode = VirtualMemory::Mode::NoAccess;
	VirtualMemory::CodeReadLease first_lease;
	if (!VirtualMemory::Protect(first, page_size, VirtualMemory::Mode::ExecuteRead, &old_mode) ||
	    !VirtualMemory::TryAcquireCodeReadLease(first, 1, &first_lease))
	{
		VirtualMemory::ReleaseCodeReadLease(&first_lease);
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}
	const uint64_t first_identity = first_lease.MappingIdentity();
	VirtualMemory::ReleaseCodeReadLease(&first_lease);

	const uint64_t                     collision_generation = VirtualMemory::GetMutationGeneration();
	VirtualMemory::MutationTransaction collision;
	VirtualMemory::MutationTransaction nested;
	VirtualMemory::MutationTransaction partial;
	VirtualMemory::MutationTransaction crossing;
	VirtualMemory::MutationTransaction adjacent;
	if (VirtualMemory::BeginAllocFixedInReservation(first, page_size, VirtualMemory::Mode::ReadWrite, &collision) !=
	        VirtualMemory::ReservedFixedResult::Failed ||
	    VirtualMemory::BeginAllocFixedInReservation(sibling + page_size, page_size, VirtualMemory::Mode::ReadWrite, &nested) !=
	        VirtualMemory::ReservedFixedResult::Failed ||
	    VirtualMemory::BeginAllocFixedInReservation(sibling - page_size, 2 * page_size, VirtualMemory::Mode::ReadWrite, &partial) !=
	        VirtualMemory::ReservedFixedResult::Failed ||
	    VirtualMemory::BeginAllocFixedInReservation(reservation.base + 7 * page_size, 2 * page_size, VirtualMemory::Mode::ReadWrite,
	                                                &crossing) != VirtualMemory::ReservedFixedResult::Failed ||
	    VirtualMemory::BeginAllocFixedInReservation(sibling - page_size, page_size, VirtualMemory::Mode::ReadWrite, &adjacent) !=
	        VirtualMemory::ReservedFixedResult::Begun ||
	    !adjacent.Abort() || collision.IsActive() || nested.IsActive() || partial.IsActive() || crossing.IsActive() ||
	    adjacent.IsActive() || VirtualMemory::GetMutationGeneration() != collision_generation + 2 || !VirtualMemory::Free(first) ||
	    sibling_bytes[0] != 0xa5 || VirtualMemory::QueryManagedRegion(first, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    VirtualMemory::QueryManagedRegion(sibling, &region) != VirtualMemory::ManagedRegionState::Visible)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}

	VirtualMemory::MutationTransaction reactivation;
	if (VirtualMemory::BeginAllocFixedInReservation(first, page_size, VirtualMemory::Mode::ReadWrite, &reactivation) !=
	    VirtualMemory::ReservedFixedResult::Begun)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}
	reactivation.Commit();
	if (first_bytes[0] != 0 || sibling_bytes[0] != 0xa5 ||
	    !VirtualMemory::Protect(first, page_size, VirtualMemory::Mode::ExecuteRead, &old_mode))
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}
	VirtualMemory::CodeReadLease second_lease;
	const bool                   new_identity =
	    VirtualMemory::TryAcquireCodeReadLease(first, 1, &second_lease) && second_lease.MappingIdentity() != first_identity;
	VirtualMemory::ReleaseCodeReadLease(&second_lease);
	if (!new_identity || VirtualMemory::QueryManagedRegion(reservation.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    region.begin != reservation.base || region.end != first ||
	    VirtualMemory::QueryManagedRegion(first, &region) != VirtualMemory::ManagedRegionState::Visible || region.begin != first ||
	    region.end != first + page_size ||
	    VirtualMemory::QueryManagedRegion(first + page_size, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    region.begin != first + page_size || region.end != sibling ||
	    VirtualMemory::QueryManagedRegion(sibling + 2 * page_size, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    region.begin != sibling + 2 * page_size || region.end != reservation.base + reservation.size)
	{
		(void)VirtualMemory::ReleaseReservation(reservation);
		return false;
	}

	const VirtualMemory::ReservationHandle stale = reservation;
	if (!VirtualMemory::ReleaseReservation(reservation) ||
	    VirtualMemory::QueryManagedRegion(stale.base, &region) != VirtualMemory::ManagedRegionState::Unmanaged)
	{
		return false;
	}

	VirtualMemory::ReservationHandle   replacement {};
	VirtualMemory::MutationTransaction exact;
	if (!VirtualMemory::BeginReserveAligned(stale.base, stale.size, alignment, &replacement, &exact))
	{
		return false;
	}
	exact.Commit();
	if (replacement.identity == stale.identity || VirtualMemory::ReleaseReservation(stale) ||
	    VirtualMemory::QueryManagedRegion(replacement.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    !VirtualMemory::ReleaseReservation(replacement))
	{
		(void)VirtualMemory::ReleaseReservation(replacement);
		return false;
	}

	constexpr uint64_t                 large_size = uint64_t {2} * 1024 * 1024 * 1024;
	constexpr uint64_t                 large_preferred_address = UINT64_C(0x1000000000);
	const uint64_t                     large_alignment = (std::max)(page_size, UINT64_C(0x4000));
	VirtualMemory::ReservationHandle   large {};
	VirtualMemory::MutationTransaction large_transaction;
	if (!VirtualMemory::BeginReserveAligned(large_preferred_address, large_size, large_alignment, &large,
	                                        &large_transaction, VirtualMemory::ReservationPlacement::PreferAddress))
	{
		return false;
	}
	large_transaction.Commit();
	return large.base % large_alignment == 0 && !VirtualMemory::IsManagedAddress(large.base) &&
	       VirtualMemory::QueryManagedRegion(large.base, &region) == VirtualMemory::ManagedRegionState::Reserved &&
	       VirtualMemory::ReleaseReservation(large);
}

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
struct GuestDataPhysicalValidationPause
{
	std::atomic_bool entered {false};
	std::atomic_bool release {false};
	std::atomic_bool timed_out {false};
};

thread_local GuestDataPhysicalValidationPause* g_guest_data_validation_pause = nullptr;

void PauseGuestDataPhysicalValidation()
{
	auto* pause = g_guest_data_validation_pause;
	if (pause == nullptr) return;
	pause->entered.store(true, std::memory_order_release);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (!pause->release.load(std::memory_order_acquire))
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			pause->timed_out.store(true, std::memory_order_release);
			break;
		}
		std::this_thread::yield();
	}
}

bool WaitGuestDataFlag(const std::atomic_bool& flag)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!flag.load(std::memory_order_acquire))
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::yield();
	}
	return true;
}

bool TestGuestDataConcurrency(uint64_t page_size)
{
	const uint64_t first = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ReadWrite);
	const uint64_t second = VirtualMemory::Alloc(0, 2 * page_size, VirtualMemory::Mode::ReadWrite);
	auto cleanup = [&]
	{
		const bool first_freed = first == 0 || VirtualMemory::Free(first);
		const bool second_freed = second == 0 || VirtualMemory::Free(second);
		return first_freed && second_freed;
	};
	if (first == 0 || second == 0)
	{
		(void)cleanup();
		return false;
	}
	constexpr uint64_t first_value = UINT64_C(0x12345678abcdef01);
	constexpr uint64_t second_value = UINT64_C(0xfedcba9876543210);
	std::memcpy(reinterpret_cast<void*>(first), &first_value, sizeof(first_value));
	std::memcpy(reinterpret_cast<void*>(second), &second_value, sizeof(second_value));
	if (!VirtualMemory::Protect(second, page_size, VirtualMemory::Mode::Read) ||
	    !VirtualMemory::Protect(second + page_size, page_size, VirtualMemory::Mode::NoAccess))
	{
		(void)cleanup();
		return false;
	}

	GuestDataPhysicalValidationPause pause;
	bool first_result = false;
	std::thread first_reader([&]
	{
		g_guest_data_validation_pause = &pause;
		VirtualMemory::SetGuestDataPhysicalValidationHookForTest(PauseGuestDataPhysicalValidation);
		{
			VirtualMemory::GuestDataLease lease;
			first_result = VirtualMemory::TryAcquireGuestDataLease(
			    first, sizeof(first_value), VirtualMemory::Mode::Read, &lease);
			if (first_result)
			{
				uint64_t observed = 0;
				std::memcpy(&observed, reinterpret_cast<const void*>(lease.HostBegin()), sizeof(observed));
				first_result = observed == first_value && lease.HostBegin() == first && lease.ByteCount() == sizeof(first_value);
			}
		}
		VirtualMemory::SetGuestDataPhysicalValidationHookForTest(nullptr);
		g_guest_data_validation_pause = nullptr;
	});
	if (!WaitGuestDataFlag(pause.entered))
	{
		pause.release.store(true, std::memory_order_release);
		first_reader.join();
		(void)cleanup();
		return false;
	}

	std::atomic_bool second_done {false};
	bool second_result = false;
	std::thread second_reader([&]
	{
		// No hook is installed on this thread. An unrelated physical read must
		// finish while the first reader remains parked in its validation hook.
		{
			VirtualMemory::GuestDataLease lease;
			second_result = VirtualMemory::TryAcquireGuestDataLease(
			    second, sizeof(second_value), VirtualMemory::Mode::Read, &lease);
			if (second_result)
			{
				uint64_t observed = 0;
				std::memcpy(&observed, reinterpret_cast<const void*>(lease.HostBegin()), sizeof(observed));
				second_result = observed == second_value && lease.HostBegin() == second && lease.ByteCount() == sizeof(second_value);
			}
			VirtualMemory::GuestDataLease denied_write;
			VirtualMemory::GuestDataLease denied_read;
			VirtualMemory::GuestDataLease denied_crossing;
			second_result = !VirtualMemory::TryAcquireGuestDataLease(
			                    second, sizeof(second_value), VirtualMemory::Mode::Write, &denied_write) &&
			                !denied_write.IsActive() && second_result;
			second_result = !VirtualMemory::TryAcquireGuestDataLease(
			                    second + page_size, 1, VirtualMemory::Mode::Read, &denied_read) &&
			                !denied_read.IsActive() && second_result;
			second_result = !VirtualMemory::TryAcquireGuestDataLease(
			                    second + page_size - 4, 8, VirtualMemory::Mode::Read, &denied_crossing) &&
			                !denied_crossing.IsActive() && second_result;
		}
		// Publish only after every lease has been released on its owner thread.
		second_done.store(true, std::memory_order_release);
	});
	const bool independent_progress = WaitGuestDataFlag(second_done);
	pause.release.store(true, std::memory_order_release);
	first_reader.join();
	second_reader.join();

	bool pending_denied = false;
	bool writer_result = false;
	std::thread writer;
	{
		VirtualMemory::GuestDataLease held;
		if (VirtualMemory::TryAcquireGuestDataLease(first, 8, VirtualMemory::Mode::Read, &held))
		{
			writer = std::thread([&] {
				VirtualMemory::ReservationHandle reservation {};
				VirtualMemory::MutationTransaction transaction;
				writer_result = VirtualMemory::BeginReserveAligned(0, page_size, page_size, &reservation, &transaction);
				if (writer_result) { transaction.Commit(); writer_result = VirtualMemory::ReleaseReservation(reservation); }
			});
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (VirtualMemory::GetVirtualRangeTransactionStateForTest().waiting_mutations != 0)
				{
					VirtualMemory::GuestDataLease denied;
					VirtualMemory::GuestDataLeaseDenialMetadata denial;
					const bool acquired = VirtualMemory::TryAcquireGuestDataLease(first, 8, VirtualMemory::Mode::Read, &denied, false, &denial);
					// The writer may reacquire the mutex after the state probe.
					// Keep the existing bounded wait until the pending state is
					// observed under the acquisition's own lock, rather than
					// confusing a legitimate try_lock denial with that category.
					if (!acquired && !denied.IsActive() &&
					    denial.reason == VirtualMemory::GuestDataLeaseDenialReason::GateTrylockUnavailable &&
					    !denial.gate_observed && !denial.mutation_active && denial.waiting_mutations == UINT64_MAX &&
					    denial.code_readers == UINT64_MAX && !denial.physical_observed)
					{
						std::this_thread::yield();
						continue;
					}
					pending_denied = !acquired &&
					    !denied.IsActive() && denial.reason == VirtualMemory::GuestDataLeaseDenialReason::GateWriterPending &&
					    denial.address == first && denial.bytes == 8 && denial.required == VirtualMemory::Mode::Read &&
					    denial.gate_observed && !denial.mutation_active && denial.waiting_mutations != 0 &&
					    denial.waiting_mutations != UINT64_MAX && denial.code_readers != 0 && denial.code_readers != UINT64_MAX &&
					    !denial.physical_observed && denial.acquisition_thread != 0;
					break;
				}
				std::this_thread::yield();
			}
		}
	}
	if (writer.joinable()) writer.join();
	uint64_t final_first = 0;
	uint64_t final_second = 0;
	std::memcpy(&final_first, reinterpret_cast<const void*>(first), sizeof(final_first));
	std::memcpy(&final_second, reinterpret_cast<const void*>(second), sizeof(final_second));
	const bool permissions = HasProtection(first, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE) &&
	                         HasProtection(second, VM_PROT_READ, VM_PROT_WRITE | VM_PROT_EXECUTE) &&
	                         HasProtection(second + page_size, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
	const bool ok = independent_progress && pending_denied && writer_result &&
	                !pause.timed_out.load(std::memory_order_acquire) && first_result && second_result &&
	                final_first == first_value && final_second == second_value && permissions;
	return cleanup() && ok;
}

bool TestTransactionOwnerGuestData(uint64_t page_size)
{
	const uint64_t data = VirtualMemory::Alloc(0, 3 * page_size, VirtualMemory::Mode::ReadWrite);
	const uint64_t code = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ExecuteRead);
	const uint64_t tiny = VirtualMemory::Alloc(0, 1, VirtualMemory::Mode::ReadWrite);
	auto cleanup = [&]
	{
		const bool data_freed = data == 0 || VirtualMemory::Free(data);
		const bool code_freed = code == 0 || VirtualMemory::Free(code);
		const bool tiny_freed = tiny == 0 || VirtualMemory::Free(tiny);
		return data_freed && code_freed && tiny_freed;
	};
	if (data == 0 || code == 0 || tiny == 0 ||
	    !VirtualMemory::Protect(data + page_size, page_size, VirtualMemory::Mode::Read) ||
	    !VirtualMemory::Protect(data + 2 * page_size, page_size, VirtualMemory::Mode::NoAccess))
	{
		(void)cleanup();
		return false;
	}

	const uint64_t output = data + 64;
	bool ok = true;
	for (const bool abort : {false, true})
	{
		VirtualMemory::ReservationHandle reservation {};
		VirtualMemory::MutationTransaction transaction;
		if (!VirtualMemory::BeginReserveAligned(0, 2 * page_size, page_size, &reservation, &transaction))
		{
			(void)cleanup();
			return false;
		}

		// A no-wait borrow beneath another subsystem lock must reject even
		// the mutation owner; only the default publication path has that exception.
		{
			VirtualMemory::GuestDataLease denied;
			VirtualMemory::GuestDataLeaseDenialMetadata denial;
			ok = !VirtualMemory::TryAcquireGuestDataLease(output, 8, VirtualMemory::Mode::Read, &denied, false, &denial) &&
			    !denied.IsActive() && denial.reason == VirtualMemory::GuestDataLeaseDenialReason::GateMutationActive &&
			    denial.address == output && denial.bytes == 8 && denial.required == VirtualMemory::Mode::Read &&
			    denial.gate_observed && denial.mutation_active && denial.waiting_mutations == 0 &&
			    denial.code_readers == 0 && !denial.physical_observed && denial.acquisition_thread != 0 && ok;
		}
		// This is the HLE result-publication order: the owner must borrow an
		// independent output while its reservation is still rollback-capable.
		{
			VirtualMemory::GuestDataLease write;
			VirtualMemory::GuestDataLease read;
			const bool write_acquired = VirtualMemory::TryAcquireGuestDataLease(
			    output, sizeof(reservation.base), VirtualMemory::Mode::Write, &write);
			const bool read_acquired = VirtualMemory::TryAcquireGuestDataLease(
			    output, sizeof(reservation.base), VirtualMemory::Mode::Read, &read);
			ok = write_acquired && read_acquired && write.HostBegin() == output &&
			     write.ByteCount() == sizeof(reservation.base) && read.HostBegin() == output && ok;
			if (write_acquired && read_acquired)
			{
				std::memcpy(reinterpret_cast<void*>(write.HostBegin()), &reservation.base, sizeof(reservation.base));
				uint64_t observed = 0;
				std::memcpy(&observed, reinterpret_cast<const void*>(read.HostBegin()), sizeof(observed));
				ok = observed == reservation.base && ok;
			}
			VirtualMemory::CodeReadLease denied_code;
			ok = !VirtualMemory::TryAcquireCodeReadLease(code, 1, &denied_code) && !denied_code.IsActive() && ok;

			struct DeniedSpan { uint64_t address; uint64_t size; VirtualMemory::Mode mode; };
			const DeniedSpan denied_spans[] = {
			    {data + page_size, 8, VirtualMemory::Mode::Write},
			    {data + page_size - 4, 8, VirtualMemory::Mode::Write},
			    {data + 2 * page_size, 8, VirtualMemory::Mode::Read},
			    {reservation.base, 8, VirtualMemory::Mode::Read},
			    {UINT64_MAX - 3, 8, VirtualMemory::Mode::Write},
			    {tiny, 2, VirtualMemory::Mode::Write},
			    {output, 0, VirtualMemory::Mode::Read},
			    {output, 8, VirtualMemory::Mode::Execute},
			};
			uint64_t before_errors = 0;
			std::memcpy(&before_errors, reinterpret_cast<const void*>(output), sizeof(before_errors));
			for (const auto& span: denied_spans)
			{
				VirtualMemory::GuestDataLease denied;
				ok = !VirtualMemory::TryAcquireGuestDataLease(span.address, span.size, span.mode, &denied) &&
				     !denied.IsActive() && ok;
			}
			uint64_t after_errors = 0;
			std::memcpy(&after_errors, reinterpret_cast<const void*>(output), sizeof(after_errors));
			ok = before_errors == after_errors && ok;
		}

		// Observe the peer inside the gate wait instead of relying on a sleep or
		// on a flag set before the peer has actually tried to acquire its lease.
		std::atomic_bool peer_done {false};
		std::atomic_bool no_wait_denied {false};
		bool peer_result = false;
		uint64_t peer_output = 0;
		std::thread peer([&]
		{
			{
				VirtualMemory::GuestDataLease denied;
				const bool acquired = VirtualMemory::TryAcquireGuestDataLease(
				    output, sizeof(peer_output), VirtualMemory::Mode::Read, &denied, false);
				no_wait_denied.store(!acquired && !denied.IsActive(), std::memory_order_release);
			}
			VirtualMemory::GuestDataLease lease;
			peer_result = VirtualMemory::TryAcquireGuestDataLease(
			    output, sizeof(peer_output), VirtualMemory::Mode::Read, &lease);
			if (peer_result)
				std::memcpy(&peer_output, reinterpret_cast<const void*>(lease.HostBegin()), sizeof(peer_output));
			peer_done.store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		bool peer_waiting = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (VirtualMemory::GetVirtualRangeTransactionStateForTest().waiting_guest_data_readers == 1)
			{
				peer_waiting = true;
				break;
			}
			if (peer_done.load(std::memory_order_acquire)) break;
			std::this_thread::yield();
		}
		ok = peer_waiting && no_wait_denied.load(std::memory_order_acquire) &&
		     !peer_done.load(std::memory_order_acquire) && ok;
		if (abort) ok = transaction.Abort() && ok;
		else transaction.Commit();
		peer.join();
		VirtualMemory::ManagedRegion region {};
		const auto expected = abort ? VirtualMemory::ManagedRegionState::Unmanaged : VirtualMemory::ManagedRegionState::Reserved;
		ok = !transaction.IsActive() && peer_result && peer_output == reservation.base &&
		     VirtualMemory::GetVirtualRangeTransactionStateForTest().waiting_guest_data_readers == 0 &&
		     VirtualMemory::QueryManagedRegion(reservation.base, &region) == expected && ok;
		VirtualMemory::CodeReadLease after_close;
		ok = VirtualMemory::TryAcquireCodeReadLease(code, 1, &after_close) && ok;
		VirtualMemory::ReleaseCodeReadLease(&after_close);
		if (!abort) ok = VirtualMemory::ReleaseReservation(reservation) && ok;
	}
	return cleanup() && ok;
}

bool GuestDataDenialMetadataIsEmpty(const VirtualMemory::GuestDataLeaseDenialMetadata& denial)
{
	return denial.reason == VirtualMemory::GuestDataLeaseDenialReason::None && denial.address == 0 && denial.bytes == 0 &&
	    denial.required == VirtualMemory::Mode::NoAccess && denial.acquisition_thread == 0 && denial.generation == UINT64_MAX &&
	    !denial.gate_observed && !denial.mutation_active && denial.waiting_mutations == UINT64_MAX &&
	    denial.code_readers == UINT64_MAX && !denial.physical_observed && denial.physical_result == 0 && denial.physical_protection == 0;
}

VirtualMemory::GuestDataLeaseDenialMetadata PoisonGuestDataDenialMetadata()
{
	VirtualMemory::GuestDataLeaseDenialMetadata denial;
	denial.reason = VirtualMemory::GuestDataLeaseDenialReason::GateMutationActive;
	denial.address = denial.bytes = denial.acquisition_thread = 123;
	denial.required = VirtualMemory::Mode::Execute;
	denial.generation = denial.waiting_mutations = denial.code_readers = 123;
	denial.gate_observed = denial.mutation_active = denial.physical_observed = true;
	denial.physical_result = 123; denial.physical_protection = 123;
	return denial;
}

struct NestedGuestDataDenialProbe
{
	bool valid = false;
	VirtualMemory::GuestDataLeaseDenialMetadata inner;
};
thread_local NestedGuestDataDenialProbe* g_nested_guest_data_denial_probe = nullptr;

void ProbeNestedGuestDataDenial()
{
	VirtualMemory::SetGuestDataPhysicalValidationHookForTest(nullptr);
	auto* probe = g_nested_guest_data_denial_probe;
	if (probe == nullptr) return;
	VirtualMemory::GuestDataLease denied;
	probe->inner = PoisonGuestDataDenialMetadata();
	probe->valid = !VirtualMemory::TryAcquireGuestDataLease(0, 8, VirtualMemory::Mode::Read, &denied, false, &probe->inner) &&
	    !denied.IsActive() && probe->inner.reason == VirtualMemory::GuestDataLeaseDenialReason::LeaseOrRangeInvalid &&
	    probe->inner.address == 0 && probe->inner.bytes == 8 && !probe->inner.gate_observed &&
	    probe->inner.waiting_mutations == UINT64_MAX && !probe->inner.physical_observed;
}

bool TestGuestDataDenialMetadata(uint64_t owned)
{
	using namespace VirtualMemory;
	struct Invalid { uint64_t address, bytes; Mode mode; };
	for (const auto& invalid: {Invalid {0, 8, Mode::Read}, Invalid {owned, 0, Mode::Read},
	                          Invalid {UINT64_MAX - 3, 8, Mode::Read}, Invalid {owned, 8, Mode::Execute}})
	{
		GuestDataLease lease;
		auto denial = PoisonGuestDataDenialMetadata();
		if (TryAcquireGuestDataLease(invalid.address, invalid.bytes, invalid.mode, &lease, false, &denial) ||
		    lease.IsActive() || denial.reason != GuestDataLeaseDenialReason::LeaseOrRangeInvalid ||
		    denial.address != invalid.address || denial.bytes != invalid.bytes || denial.required != invalid.mode ||
		    denial.acquisition_thread == 0 || denial.generation != UINT64_MAX || denial.gate_observed || denial.mutation_active ||
		    denial.waiting_mutations != UINT64_MAX || denial.code_readers != UINT64_MAX || denial.physical_observed ||
		    denial.physical_result != 0 || denial.physical_protection != 0) return false;
	}
	{
		GuestDataLease lease;
		auto denial = PoisonGuestDataDenialMetadata();
		if (TryAcquireGuestDataLease(owned, 8, Mode::Read, nullptr, false, &denial) ||
		    denial.reason != GuestDataLeaseDenialReason::LeaseOrRangeInvalid || denial.gate_observed) return false;
		if (!TryAcquireGuestDataLease(owned, 8, Mode::Read, &lease, false, &denial) || !GuestDataDenialMetadataIsEmpty(denial)) return false;
		if (TryAcquireGuestDataLease(owned, 8, Mode::Read, &lease, false, &denial) ||
		    denial.reason != GuestDataLeaseDenialReason::LeaseOrRangeInvalid || denial.gate_observed || !lease.IsActive()) return false;
	}
	NestedGuestDataDenialProbe probe;
	g_nested_guest_data_denial_probe = &probe;
	SetGuestDataPhysicalValidationHookForTest(ProbeNestedGuestDataDenial);
	auto outer = PoisonGuestDataDenialMetadata();
	bool acquired = false;
	{
		GuestDataLease lease;
		acquired = TryAcquireGuestDataLease(owned, 8, Mode::Read, &lease, false, &outer) && lease.IsActive();
	}
	SetGuestDataPhysicalValidationHookForTest(nullptr);
	g_nested_guest_data_denial_probe = nullptr;
	if (!acquired || !probe.valid || !GuestDataDenialMetadataIsEmpty(outer) ||
	    probe.inner.reason != GuestDataLeaseDenialReason::LeaseOrRangeInvalid) return false;
	std::puts("[GUEST_DATA_DENIAL_METADATA_TEST] invalid/reset/success/nested=pass observation-only=1 authority=unchanged");
	return true;
}

bool ExpectGuestDataLease(const char* name, uint64_t address, uint64_t size,
                         VirtualMemory::Mode mode, bool expected,
                         VirtualMemory::GuestDataLeaseDenialReason expected_denial = VirtualMemory::GuestDataLeaseDenialReason::None,
                         uint32_t expected_physical_protection = UINT32_MAX)
{
	VirtualMemory::GuestDataLease lease;
	auto denial = PoisonGuestDataDenialMetadata();
	const bool acquired = VirtualMemory::TryAcquireGuestDataLease(address, size, mode, &lease, true, &denial);
	VirtualMemory::GuestDataLease no_wait;
	auto immediate_denial = PoisonGuestDataDenialMetadata();
	const bool immediate = VirtualMemory::TryAcquireGuestDataLease(address, size, mode, &no_wait, false, &immediate_denial);
	const auto metadata = [&](const VirtualMemory::GuestDataLeaseDenialMetadata& observed) {
		using Reason = VirtualMemory::GuestDataLeaseDenialReason;
		if (expected) return GuestDataDenialMetadataIsEmpty(observed);
		return observed.address == address && observed.bytes == size && observed.required == mode &&
		    observed.acquisition_thread != 0 && observed.generation != UINT64_MAX && observed.gate_observed &&
		    !observed.mutation_active && observed.waiting_mutations == 0 &&
		    (observed.reason == Reason::LogicalSnapshotDenied || observed.reason == Reason::LogicalRegistryDenied ||
		     observed.reason == Reason::PhysicalQueryOrRightsDenied) &&
		    (observed.reason == Reason::PhysicalQueryOrRightsDenied || !observed.physical_observed) &&
		    (expected_denial == Reason::None || observed.reason == expected_denial) &&
		    (expected_physical_protection == UINT32_MAX ||
		     (observed.physical_observed && observed.physical_result == KERN_SUCCESS &&
		      observed.physical_protection == expected_physical_protection));
	};
	const bool ok = acquired == expected && lease.IsActive() == expected &&
	                lease.HostBegin() == (expected ? address : 0) &&
	                lease.ByteCount() == (expected ? size : 0) && immediate == expected &&
	                no_wait.IsActive() == expected && no_wait.HostBegin() == (expected ? address : 0) &&
	                no_wait.ByteCount() == (expected ? size : 0) && metadata(denial) && metadata(immediate_denial);
	if (!ok) std::printf("guest-data-snapshot-preflight case=%s expected=%d acquired=%d\n", name, expected, acquired);
	return ok;
}

bool TestGuestDataSnapshotPreflight(uint64_t page_size)
{
	using Mode = VirtualMemory::Mode;
	const uint64_t data = VirtualMemory::Alloc(0, 3 * page_size, Mode::ReadWrite);
	const uint64_t tiny = VirtualMemory::Alloc(0, 1, Mode::ReadWrite);
	void* native = mmap(nullptr, static_cast<size_t>(3 * page_size), PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANON, -1, 0);
	const uint64_t native_begin = reinterpret_cast<uint64_t>(native);
	const uint64_t center = native_begin + page_size;
	bool center_unmapped = false;
	VirtualMemory::ReservationHandle center_reservation {};
	VirtualMemory::ReservationHandle invalid_publication {};
	auto cleanup = [&]
	{
		bool ok = !invalid_publication.IsValid() || VirtualMemory::ReleaseReservation(invalid_publication);
		ok = (!center_reservation.IsValid() || VirtualMemory::ReleaseReservation(center_reservation)) && ok;
		if (native != MAP_FAILED)
		{
			if (center_unmapped)
			{
				ok = munmap(native, static_cast<size_t>(page_size)) == 0 && ok;
				ok = munmap(reinterpret_cast<void*>(center + page_size), static_cast<size_t>(page_size)) == 0 && ok;
			} else
			{
				ok = munmap(native, static_cast<size_t>(3 * page_size)) == 0 && ok;
			}
		}
		const bool data_freed = data == 0 || VirtualMemory::Free(data);
		const bool tiny_freed = tiny == 0 || VirtualMemory::Free(tiny);
		return data_freed && tiny_freed && ok;
	};
	if (data == 0 || tiny == 0 || native == MAP_FAILED ||
	    !VirtualMemory::Protect(data + page_size, page_size, Mode::Read) ||
	    !VirtualMemory::Protect(data + 2 * page_size, page_size, Mode::NoAccess))
	{
		(void)cleanup();
		return false;
	}

	bool ok = TestGuestDataDenialMetadata(data) && ExpectGuestDataLease("managed-rw", data, 8, Mode::ReadWrite, true);
	ok = ExpectGuestDataLease("cross-rw-r-read", data + page_size - 4, 8, Mode::Read, true) && ok;
	ok = ExpectGuestDataLease("cross-rw-r-write", data + page_size - 4, 8, Mode::Write, false,
	    VirtualMemory::GuestDataSnapshotPreflightEnabledForTest() ? VirtualMemory::GuestDataLeaseDenialReason::LogicalSnapshotDenied
	        : VirtualMemory::GuestDataLeaseDenialReason::LogicalRegistryDenied) && ok;
	ok = ExpectGuestDataLease("cross-r-guard", data + 2 * page_size - 4, 8, Mode::Read, false) && ok;
	ok = ExpectGuestDataLease("visible-last-byte", tiny, 1, Mode::ReadWrite, true) && ok;
	ok = ExpectGuestDataLease("cross-padding", tiny, 2, Mode::Read, false) && ok;
	ok = ExpectGuestDataLease("padding", tiny + 1, 1, Mode::Read, false) && ok;
	ok = ExpectGuestDataLease("native-range", native_begin, 3 * page_size, Mode::ReadWrite, true) && ok;

	// A successful logical preflight cannot cache permission to bypass Mach on
	// the next acquisition. These raw changes deliberately leave Core unchanged.
	const uint64_t warm_generation = VirtualMemory::GetMutationGeneration();
	if (mprotect(reinterpret_cast<void*>(data), static_cast<size_t>(page_size), PROT_READ) != 0)
	{
		(void)cleanup();
		return false;
	}
	ok = ExpectGuestDataLease("physical-r-read", data, 8, Mode::Read, true) && ok;
	ok = ExpectGuestDataLease("physical-r-write", data, 8, Mode::Write, false,
	    VirtualMemory::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied, VM_PROT_READ) && ok;
	const bool physical_guard = mprotect(reinterpret_cast<void*>(data), static_cast<size_t>(page_size), PROT_NONE) == 0;
	ok = physical_guard && ok;
	if (physical_guard) ok = ExpectGuestDataLease("physical-guard", data, 8, Mode::Read, false,
	    VirtualMemory::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied, VM_PROT_NONE) && ok;
	const bool physical_restored = mprotect(reinterpret_cast<void*>(data), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) == 0;
	ok = physical_restored && VirtualMemory::GetMutationGeneration() == warm_generation && ok;
	if (physical_restored) ok = ExpectGuestDataLease("physical-restored", data, 8, Mode::ReadWrite, true) && ok;

	if (munmap(reinterpret_cast<void*>(center), static_cast<size_t>(page_size)) != 0)
	{
		(void)cleanup();
		return false;
	}
	center_unmapped = true;
	const uint64_t crossing = center - 8;
	const uint64_t crossing_size = page_size + 16;
	ok = ExpectGuestDataLease("native-hole-native", crossing, crossing_size, Mode::Read, false) && ok;
	{
		VirtualMemory::MutationTransaction transaction;
		if (!VirtualMemory::BeginReserveAligned(center, page_size, page_size, &center_reservation, &transaction))
		{
			(void)cleanup();
			return false;
		}
		transaction.Commit();
	}
	// Widen only the physical backing. A native prefix must not skip a later
	// logical reservation/guard even when Mach can see readable bytes there.
	if (mprotect(reinterpret_cast<void*>(center), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) != 0)
	{
		(void)cleanup();
		return false;
	}
	ok = ExpectGuestDataLease("native-reserved-native", crossing, crossing_size, Mode::Read, false) && ok;
	if (!VirtualMemory::AllocFixed(center, page_size, Mode::ReadWrite))
	{
		(void)cleanup();
		return false;
	}
	ok = ExpectGuestDataLease("native-managed-native", crossing, crossing_size, Mode::ReadWrite, true) && ok;
	for (const auto mode : {Mode::Read, Mode::NoAccess})
	{
		if (!VirtualMemory::Protect(center, page_size, mode) ||
		    mprotect(reinterpret_cast<void*>(center), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) != 0)
		{
			(void)cleanup();
			return false;
		}
		ok = ExpectGuestDataLease("native-managed-read", crossing, crossing_size, Mode::Read, mode == Mode::Read) && ok;
		ok = ExpectGuestDataLease("native-managed-write", crossing, crossing_size, Mode::Write, false) && ok;
	}
	if (!VirtualMemory::Free(center))
	{
		(void)cleanup();
		return false;
	}
	ok = ExpectGuestDataLease("freed-reservation-child", center, 8, Mode::Read, false) && ok;
	if (!VirtualMemory::AllocFixed(center, page_size, Mode::Read))
	{
		(void)cleanup();
		return false;
	}
	ok = ExpectGuestDataLease("remapped-read", center, 8, Mode::Read, true) && ok;
	ok = ExpectGuestDataLease("remapped-write", center, 8, Mode::Write, false) && ok;

	// Publication failure invalidates the snapshot. Without another mutation,
	// valid data must still borrow via the registry and invalid spans must deny.
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::SnapshotPublish);
	{
		VirtualMemory::MutationTransaction transaction;
		if (!VirtualMemory::BeginReserveAligned(0, page_size, page_size, &invalid_publication, &transaction))
		{
			(void)cleanup();
			return false;
		}
		transaction.Commit();
	}
	VirtualMemory::ManagedRegion snapshot {};
	auto state = VirtualMemory::ManagedRegionState::Unmanaged;
	ok = !VirtualMemory::TryQueryManagedRegionSnapshot(data, &state, &snapshot) && ok;
	ok = ExpectGuestDataLease("invalid-snapshot-managed", data, 8, Mode::ReadWrite, true) && ok;
	ok = ExpectGuestDataLease("invalid-snapshot-native", native_begin, 8, Mode::ReadWrite, true) && ok;
	ok = ExpectGuestDataLease("invalid-snapshot-readonly", data + page_size, 8, Mode::Write, false,
	    VirtualMemory::GuestDataLeaseDenialReason::LogicalRegistryDenied) && ok;
	ok = ExpectGuestDataLease("invalid-snapshot-padding", tiny, 2, Mode::Read, false) && ok;
	ok = ExpectGuestDataLease("invalid-snapshot-reserved", invalid_publication.base, 8, Mode::Read, false) && ok;
	return cleanup() && ok;
}

uint64_t g_local_release_probe = 0;
uint32_t g_local_release_calls = 0;
bool g_local_release_valid = true;

void CheckLocalMutationRelease()
{
	++g_local_release_calls;
	VirtualMemory::CodeReadLease lease;
	const bool acquired = VirtualMemory::TryAcquireCodeReadLease(g_local_release_probe, 1, &lease);
	g_local_release_valid = acquired && g_local_release_valid;
	VirtualMemory::ReleaseCodeReadLease(&lease);
}

bool TestMutationLocalReleasePhase(uint64_t page_size)
{
	g_local_release_probe = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ExecuteRead);
	if (g_local_release_probe == 0) return false;
	g_local_release_calls = 0;
	g_local_release_valid = true;
	VirtualMemory::SetMemoryMutationLocalReleaseHookForTest(&CheckLocalMutationRelease);
	uint32_t expected_calls = 0;
	const auto released = [&] { return g_local_release_valid && g_local_release_calls == ++expected_calls; };
	bool ok = true;
	const uint64_t data = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ReadWrite);
	ok = data != 0 && released() && ok;
	if (data != 0)
	{
		ok = VirtualMemory::Protect(data, page_size, VirtualMemory::Mode::Read) && released() && ok;
		ok = VirtualMemory::PatchReplace(data, 0x13579bdf2468ace0ULL) && released() && ok;
		ok = VirtualMemory::Free(data) && released() && ok;
	}
	for (unsigned terminal = 0; terminal != 3; ++terminal)
	{
		VirtualMemory::ReservationHandle reservation {};
		const uint32_t before = g_local_release_calls;
		{
			VirtualMemory::MutationTransaction transaction;
			const bool began = VirtualMemory::BeginReserveAligned(0, page_size, page_size, &reservation, &transaction);
			ok = began && g_local_release_calls == before && ok;
			if (began)
			{
				VirtualMemory::CodeReadLease denied;
				ok = !VirtualMemory::TryAcquireCodeReadLease(g_local_release_probe, 1, &denied) && ok;
				VirtualMemory::ReleaseCodeReadLease(&denied);
				if (terminal == 0) transaction.Commit();
				if (terminal == 1) ok = transaction.Abort() && ok;
				// terminal 2 exercises destructor rollback.
			}
		}
		ok = released() && ok;
		if (terminal == 0 && reservation.IsValid())
			ok = VirtualMemory::ReleaseReservation(reservation) && released() && ok;
	}
	VirtualMemory::SetMemoryMutationLocalReleaseHookForTest(nullptr);
	const bool freed = VirtualMemory::Free(g_local_release_probe);
	g_local_release_probe = 0;
	return ok && freed;
}

bool TestReservationTransactionLifetime(uint64_t page_size)
{
	const uint64_t probe = VirtualMemory::Alloc(0, page_size, VirtualMemory::Mode::ExecuteRead);
	if (probe == 0) return false;

	const auto                         before_commit = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	VirtualMemory::ReservationHandle   committed {};
	VirtualMemory::MutationTransaction commit_transaction;
	if (!VirtualMemory::BeginReserveAligned(0, 2 * page_size, page_size, &committed, &commit_transaction))
	{
		(void)VirtualMemory::Free(probe);
		return false;
	}
	const auto                   active_commit = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	VirtualMemory::CodeReadLease denied_lease;
	const bool                   reader_denied = !VirtualMemory::TryAcquireCodeReadLease(probe, 1, &denied_lease);
	commit_transaction.Commit();
	const auto                   after_commit = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	VirtualMemory::CodeReadLease admitted_lease;
	const bool                   reader_admitted = VirtualMemory::TryAcquireCodeReadLease(probe, 1, &admitted_lease);
	VirtualMemory::ReleaseCodeReadLease(&admitted_lease);
	if (!reader_denied || !reader_admitted || active_commit.begins != before_commit.begins + 1 ||
	    active_commit.closes != before_commit.closes || active_commit.active != before_commit.active + 1 ||
	    after_commit.begins != before_commit.begins + 1 || after_commit.closes != before_commit.closes + 1 ||
	    after_commit.closes_while_gate_owned != before_commit.closes_while_gate_owned + 1 || after_commit.active != before_commit.active ||
	    !VirtualMemory::ReleaseReservation(committed))
	{
		(void)VirtualMemory::ReleaseReservation(committed);
		(void)VirtualMemory::Free(probe);
		return false;
	}

	const auto                         before_abort = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	VirtualMemory::ReservationHandle   aborted {};
	VirtualMemory::MutationTransaction abort_transaction;
	if (!VirtualMemory::BeginReserveAligned(0, 2 * page_size, page_size, &aborted, &abort_transaction) || !abort_transaction.Abort())
	{
		(void)VirtualMemory::ReleaseReservation(aborted);
		(void)VirtualMemory::Free(probe);
		return false;
	}
	const auto after_abort = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	if (after_abort.begins != before_abort.begins + 1 || after_abort.closes != before_abort.closes + 1 ||
	    after_abort.closes_while_gate_owned != before_abort.closes_while_gate_owned + 1 || after_abort.active != before_abort.active)
	{
		(void)VirtualMemory::Free(probe);
		return false;
	}

	const auto before_denial = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::FexAdditiveAdmission);
	VirtualMemory::ReservationHandle   denied {};
	VirtualMemory::MutationTransaction denied_transaction;
	const bool denied_result = VirtualMemory::BeginReserveAligned(0, 2 * page_size, page_size, &denied, &denied_transaction);
	const auto after_denial  = VirtualMemory::GetVirtualRangeTransactionStateForTest();
	return !denied_result && !denied.IsValid() && !denied_transaction.IsActive() && after_denial.begins == before_denial.begins &&
	       after_denial.closes == before_denial.closes && after_denial.closes_while_gate_owned == before_denial.closes_while_gate_owned &&
	       after_denial.active == before_denial.active && VirtualMemory::Free(probe);
}

bool TestFixedReservationConfinement(uint64_t page_size)
{
	void* raw = mmap(nullptr, static_cast<size_t>(3 * page_size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (raw == MAP_FAILED) return false;
	const uint64_t raw_base = reinterpret_cast<uint64_t>(raw);
	const uint64_t center   = raw_base + page_size;
	auto*          prefix   = reinterpret_cast<uint8_t*>(raw);
	auto*          suffix   = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(center + page_size));
	prefix[0]               = 0x31;
	prefix[page_size - 1]   = 0x32;
	suffix[0]               = 0x41;
	suffix[page_size - 1]   = 0x42;
	if (munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(center)), static_cast<size_t>(page_size)) != 0)
	{
		(void)munmap(raw, static_cast<size_t>(3 * page_size));
		return false;
	}

	VirtualMemory::ReservationHandle   exact {};
	VirtualMemory::MutationTransaction exact_transaction;
	bool                               ok = VirtualMemory::BeginReserveAligned(center, page_size, page_size, &exact, &exact_transaction);
	if (ok) exact_transaction.Commit();
	VirtualMemory::MutationTransaction child;
	ok = ok && VirtualMemory::BeginAllocFixedInReservation(center, page_size, VirtualMemory::Mode::ReadWrite, &child) ==
	               VirtualMemory::ReservedFixedResult::Begun;
	if (child.IsActive()) child.Commit();
	if (ok)
	{
		auto* bytes          = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(center));
		bytes[0]             = 0x77;
		bytes[page_size - 1] = 0x78;
		ok                   = prefix[0] == 0x31 && prefix[page_size - 1] == 0x32 && suffix[0] == 0x41 && suffix[page_size - 1] == 0x42;
	}
	const bool exact_released = exact.IsValid() && VirtualMemory::ReleaseReservation(exact);
	ok = ok && exact_released && prefix[0] == 0x31 && prefix[page_size - 1] == 0x32 && suffix[0] == 0x41 && suffix[page_size - 1] == 0x42;
	const bool prefix_released = munmap(raw, static_cast<size_t>(page_size)) == 0;
	const bool suffix_released =
	    munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(center + page_size)), static_cast<size_t>(page_size)) == 0;
	if (!ok || !prefix_released || !suffix_released) return false;

	VirtualMemory::ReservationHandle   anywhere {};
	VirtualMemory::MutationTransaction anywhere_transaction;
	if (!VirtualMemory::BeginReserveAligned(0, page_size, 4 * page_size, &anywhere, &anywhere_transaction))
	{
		return false;
	}
	anywhere_transaction.Commit();
	const uint64_t padding = anywhere.base + anywhere.size;
	Region         padding_region {};
	if (QueryRegion(padding, &padding_region))
	{
		(void)VirtualMemory::ReleaseReservation(anywhere);
		return false;
	}
	void* reused = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(padding)), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (reused == MAP_FAILED || reinterpret_cast<uint64_t>(reused) != padding)
	{
		if (reused != MAP_FAILED) (void)munmap(reused, static_cast<size_t>(page_size));
		(void)VirtualMemory::ReleaseReservation(anywhere);
		return false;
	}
	auto* padding_bytes          = static_cast<uint8_t*>(reused);
	padding_bytes[0]             = 0x63;
	const bool anywhere_released = VirtualMemory::ReleaseReservation(anywhere);
	const bool padding_preserved = padding_bytes[0] == 0x63;
	const bool padding_released  = munmap(reused, static_cast<size_t>(page_size)) == 0;
	return anywhere_released && padding_preserved && padding_released;
}

bool TestSparseReservationFaults(uint64_t page_size)
{
	VirtualMemory::ReservationHandle   handle {};
	VirtualMemory::MutationTransaction transaction;
	const uint64_t                     initial_generation = VirtualMemory::GetMutationGeneration();
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::CoreCapacity);
	if (VirtualMemory::BeginReserveAligned(0, 4 * page_size, page_size, &handle, &transaction) || handle.IsValid() ||
	    transaction.IsActive() || VirtualMemory::GetMutationGeneration() != initial_generation ||
	    VirtualMemory::ReservationCountForTest() != 0)
	{
		return false;
	}

	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::FexAdditiveAdmission);
	if (VirtualMemory::BeginReserveAligned(0, 4 * page_size, page_size, &handle, &transaction) ||
	    VirtualMemory::GetMutationGeneration() != initial_generation || VirtualMemory::ReservationCountForTest() != 0)
	{
		return false;
	}

	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::SnapshotPublish);
	if (!VirtualMemory::BeginReserveAligned(0, 4 * page_size, page_size, &handle, &transaction))
	{
		return false;
	}
	transaction.Commit();
	VirtualMemory::ManagedRegion region {};
	if (VirtualMemory::ReservationCountForTest() != 1 ||
	    VirtualMemory::QueryManagedRegion(handle.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    !VirtualMemory::ReleaseReservation(handle))
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}

	if (!VirtualMemory::BeginReserveAligned(0, 4 * page_size, page_size, &handle, &transaction))
	{
		return false;
	}
	transaction.Commit();
	const uint64_t child           = handle.base + page_size;
	const uint64_t before_activate = VirtualMemory::GetMutationGeneration();
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::ActivateHost);
	if (VirtualMemory::BeginAllocFixedInReservation(child, page_size, VirtualMemory::Mode::ReadWrite, &transaction) !=
	        VirtualMemory::ReservedFixedResult::Failed ||
	    transaction.IsActive() || VirtualMemory::GetMutationGeneration() != before_activate)
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	if (VirtualMemory::BeginAllocFixedInReservation(child, page_size, VirtualMemory::Mode::ReadWrite, &transaction) !=
	    VirtualMemory::ReservedFixedResult::Begun)
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::AbortHost);
	if (transaction.Abort() || transaction.IsActive() ||
	    VirtualMemory::QueryManagedRegion(child, &region) != VirtualMemory::ManagedRegionState::Visible || !VirtualMemory::Free(child) ||
	    VirtualMemory::BeginAllocFixedInReservation(child, page_size, VirtualMemory::Mode::ReadWrite, &transaction) !=
	        VirtualMemory::ReservedFixedResult::Begun)
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	transaction.Commit();
	const uint64_t before_decommit = VirtualMemory::GetMutationGeneration();
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::DecommitHost);
	if (VirtualMemory::Free(child) || VirtualMemory::GetMutationGeneration() != before_decommit ||
	    VirtualMemory::QueryManagedRegion(child, &region) != VirtualMemory::ManagedRegionState::Visible || !VirtualMemory::Free(child))
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	const uint64_t before_denial = VirtualMemory::GetMutationGeneration();
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::FexExclusiveAdmission);
	if (VirtualMemory::ReleaseReservation(handle) || VirtualMemory::GetMutationGeneration() != before_denial)
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::ReleaseHost);
	if (VirtualMemory::ReleaseReservation(handle) || VirtualMemory::GetMutationGeneration() != before_denial ||
	    VirtualMemory::QueryManagedRegion(handle.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    !VirtualMemory::ReleaseReservation(handle))
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}

	if (!VirtualMemory::BeginReserveAligned(0, 2 * page_size, page_size, &handle, &transaction))
	{
		return false;
	}
	VirtualMemory::FailNextVirtualRangeOperationForTest(VirtualMemory::VirtualRangeFaultForTest::AbortHost);
	if (transaction.Abort() || transaction.IsActive() || VirtualMemory::ReservationCountForTest() != 1 ||
	    VirtualMemory::QueryManagedRegion(handle.base, &region) != VirtualMemory::ManagedRegionState::Reserved ||
	    !VirtualMemory::ReleaseReservation(handle) || VirtualMemory::ReservationCountForTest() != 0)
	{
		(void)VirtualMemory::ReleaseReservation(handle);
		return false;
	}
	return true;
}
#endif

} // namespace
#endif

int main(int argc, char** argv)
{
#if defined(__APPLE__)
	bool selector_oracle = true;
	bool transaction_guest_data_only = false;
	bool guest_data_concurrency_only = false;
	bool guest_data_snapshot_preflight_only = false;
	bool guest_data_continuity_only = false;
	bool memory_handoff_only = false;
	if (argc == 2 && std::strcmp(argv[1], "--snapshot-selector-enabled") == 0)
	{
		selector_oracle = VirtualMemory::IsImmutableRegionSnapshotEnabled();
	} else if (argc == 2 && std::strcmp(argv[1], "--snapshot-selector-disabled") == 0)
	{
		selector_oracle = !VirtualMemory::IsImmutableRegionSnapshotEnabled();
	} else if (argc == 2 && std::strcmp(argv[1], "--transaction-guest-data") == 0)
	{
		transaction_guest_data_only = true;
	} else if (argc == 2 && std::strcmp(argv[1], "--guest-data-concurrency") == 0)
	{
		guest_data_concurrency_only = true;
	} else if (argc == 2 && std::strcmp(argv[1], "--guest-data-snapshot-preflight") == 0)
	{
		guest_data_snapshot_preflight_only = true;
	} else if (argc == 2 && std::strcmp(argv[1], "--guest-data-continuity") == 0)
	{
		guest_data_continuity_only = true;
	} else if (argc == 2 && std::strcmp(argv[1], "--memory-handoff") == 0)
	{
		memory_handoff_only = true;
	} else if (argc != 1)
	{
		return 2;
	}

	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (host_page_size <= 0)
	{
		return 1;
	}
	const uint64_t page_size = static_cast<uint64_t>(host_page_size);
	if (memory_handoff_only)
	{
		VirtualMemory::Init();
		const bool shared = TestSharedPhysicalBacking(page_size);
		const bool owner = TestExternalHandoffOwner(page_size);
		std::printf("virtual-memory memory-handoff=%s shared-backing=%s external-owner=%s page=%llu\n",
		    shared && owner ? "ok" : "fail", shared ? "ok" : "fail", owner ? "ok" : "fail",
		    static_cast<unsigned long long>(page_size));
		return shared && owner ? 0 : 1;
	}
	if (guest_data_continuity_only)
	{
		VirtualMemory::Init();
		return TestGuestDataRangeContinuity(page_size) ? 0 : 1;
	}
	if (guest_data_snapshot_preflight_only)
	{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		VirtualMemory::Init();
		const char* expected_selector =
		    std::getenv("LUMEN_EXPECT_GUEST_DATA_SNAPSHOT_PREFLIGHT");
		const bool selector = expected_selector == nullptr ||
		    (std::strcmp(expected_selector, "1") == 0) ==
		        VirtualMemory::GuestDataSnapshotPreflightEnabledForTest();
		const bool spans = TestGuestDataSnapshotPreflight(page_size);
		const bool transaction = TestTransactionOwnerGuestData(page_size);
		const bool concurrency = TestGuestDataConcurrency(page_size);
		const bool ok = selector && spans && transaction && concurrency;
		std::printf("virtual-memory guest-data-snapshot-preflight=%s selector=%s effective=%u spans=%s transaction=%s concurrency=%s\n",
		            ok ? "ok" : "fail", selector ? "ok" : "fail",
		            VirtualMemory::GuestDataSnapshotPreflightEnabledForTest() ? 1U : 0U,
		            spans ? "ok" : "fail", transaction ? "ok" : "fail", concurrency ? "ok" : "fail");
		return ok ? 0 : 1;
#else
		return 2;
#endif
	}
	if (guest_data_concurrency_only)
	{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		VirtualMemory::Init();
		const bool guest_data_concurrency = TestGuestDataConcurrency(page_size);
		std::printf("virtual-memory guest-data-concurrency=%s\n", guest_data_concurrency ? "ok" : "fail");
		return guest_data_concurrency ? 0 : 1;
#else
		return 2;
#endif
	}
	if (transaction_guest_data_only)
	{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		VirtualMemory::Init();
		const bool transaction_guest_data = TestTransactionOwnerGuestData(page_size);
		std::printf("virtual-memory transaction-guest-data=%s\n", transaction_guest_data ? "ok" : "fail");
		return transaction_guest_data ? 0 : 1;
#else
		return 2;
#endif
	}

	const bool init_invalid          = TestInitAndInvalid();
	const bool generation            = TestMutationGeneration(page_size);
	const bool range_continuity      = TestGuestDataRangeContinuity(page_size);
	const bool managed_cross_thread  = TestManagedAddressCrossThreadInvalidation(page_size);
	const bool lifecycle             = TestAllocateProtectAndFree(page_size);
	const bool uniform_data          = TestUniformHostPageDataProtection(page_size);
	const bool mode_matrix           = TestProtectionModeMatrix(page_size);
	const bool invalid_modes         = TestInvalidProtectionModes(page_size);
	const bool unaligned             = TestUnalignedProtection(page_size);
	const bool guest_pages           = TestGuestPageMetadata(page_size);
	const bool aligned               = TestAlignedAndFixed(page_size);
	const bool patch                 = TestPatchReplace(page_size);
	const bool patch_race            = TestConcurrentPatch(page_size);
	const bool concurrent            = TestConcurrentLifecycle(page_size);
	const bool snapshot_differential = TestImmutableSnapshotDifferential();
	const bool snapshot_concurrent   = TestImmutableSnapshotConcurrentReaders();
	const bool snapshot_window       = TestImmutableSnapshotBoundedWindow() && TestImmutableSnapshotExecutionPinnedLookup();
	const bool code_lease            = TestCodeReadLease(page_size);
	const bool sparse_reservation    = TestSparseReservation(page_size);
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	const bool transaction_guest_data = TestTransactionOwnerGuestData(page_size);
	const bool mutation_local_release = TestMutationLocalReleasePhase(page_size);
	const bool sparse_transaction_lifetime = TestReservationTransactionLifetime(page_size);
	const bool sparse_fixed_confinement    = TestFixedReservationConfinement(page_size);
	const bool sparse_reservation_faults   = TestSparseReservationFaults(page_size);
#else
	const bool transaction_guest_data = true;
	const bool mutation_local_release = true;
	const bool sparse_transaction_lifetime = true;
	const bool sparse_fixed_confinement    = true;
	const bool sparse_reservation_faults   = true;
#endif

	std::printf("virtual-memory page=%llu init-invalid=%s generation=%s managed-cross-thread=%s lifecycle=%s unaligned=%s "
	            "guest-pages=%s aligned-fixed=%s patch=%s patch-race=%s concurrent=%s snapshot-differential=%s "
	            "snapshot-concurrent=%s snapshot-window=%s selector=%s code-lease=%s sparse-reservation=%s "
	            "sparse-transaction-lifetime=%s sparse-fixed-confinement=%s sparse-reservation-faults=%s uniform-data=%s "
	            "mode-matrix=%s invalid-modes=%s transaction-guest-data=%s mutation-local-release=%s\n",
	            static_cast<unsigned long long>(page_size), init_invalid ? "ok" : "fail", generation ? "ok" : "fail",
	            managed_cross_thread ? "ok" : "fail", lifecycle ? "ok" : "fail", unaligned ? "ok" : "fail", guest_pages ? "ok" : "fail",
	            aligned ? "ok" : "fail", patch ? "ok" : "fail", patch_race ? "ok" : "fail", concurrent ? "ok" : "fail",
	            snapshot_differential ? "ok" : "fail", snapshot_concurrent ? "ok" : "fail", snapshot_window ? "ok" : "fail",
	            selector_oracle ? "ok" : "fail", code_lease ? "ok" : "fail", sparse_reservation ? "ok" : "fail",
	            sparse_transaction_lifetime ? "ok" : "fail", sparse_fixed_confinement ? "ok" : "fail",
	            sparse_reservation_faults ? "ok" : "fail", uniform_data ? "ok" : "fail",
	            mode_matrix ? "ok" : "fail", invalid_modes ? "ok" : "fail", transaction_guest_data ? "ok" : "fail",
	            mutation_local_release ? "ok" : "fail");
	return init_invalid && generation && range_continuity && managed_cross_thread && lifecycle && unaligned && guest_pages && aligned && patch && patch_race &&
	               concurrent && snapshot_differential && snapshot_concurrent && snapshot_window && selector_oracle && code_lease &&
	               sparse_reservation && sparse_transaction_lifetime && sparse_fixed_confinement && sparse_reservation_faults && uniform_data &&
	               mode_matrix && invalid_modes && transaction_guest_data && mutation_local_release
	           ? 0
	           : 1;
#else
	std::puts("virtual-memory=skipped (requires Apple)");
	return 0;
#endif
}
