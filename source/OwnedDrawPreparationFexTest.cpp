// Lumen, 2026-10-17. SPDX-License-Identifier: GPL-2.0-only
// Liveness gate for the production owned-preparation epoch boundary. This
// exercises real FEX, VM and mutex HLE; it does not qualify a Vulkan draw or
// the completeness of the renderer's owned shader/resource snapshots.
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#include "cpu/runtime/MemoryHandoff.h"
#include "Lumen/Core/Database.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Math/Rand.h"

#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Kernel/Pthread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

namespace Lumen::Libs::Graphics {
bool GpuMemoryBackingQueryCacheContractTest(uint64_t address, uint64_t bytes, bool expected_owned);
bool GpuMemoryRangeClaimPlanContractTest(uint64_t allocation);
bool GpuMemoryNativeRangeDirtyHandoffContractTest(uint64_t claimed_address, uint64_t dirty_address);
}

namespace {

namespace VM = Lumen::Core::VirtualMemory;
namespace Kernel = Lumen::Libs::LibKernel;
namespace Graphics = Lumen::Libs::Graphics;
constexpr uint64_t kMappingBytes = 16384;
constexpr uint64_t kWrittenValue = UINT64_C(0x6f776e6564707265);
constexpr uint32_t kGuestSuccess = 0x175a;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "OwnedDrawPreparationFexTest: FAIL: %s\n", message);
	return condition;
}

// No elapsed-time result or speed threshold is used. A deadlocked admission
// must fail the executable instead of leaving a join blocked indefinitely.
class DeadlockWatchdog final
{
public:
	DeadlockWatchdog(): worker([this] {
		std::unique_lock lock(mutex);
		if (!changed.wait_for(lock, std::chrono::seconds(30), [this] { return finished; }))
		{
			std::fputs("OwnedDrawPreparationFexTest: FAIL: deadlock watchdog\n", stderr);
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}
	~DeadlockWatchdog()
	{
		{
			std::lock_guard lock(mutex);
			finished = true;
		}
		changed.notify_all();
		worker.join();
	}

private:
	std::mutex mutex;
	std::condition_variable changed;
	bool finished = false;
	std::thread worker;
};

struct Mapping final
{
	Mapping(): address(VM::Alloc(0, kMappingBytes, VM::Mode::ReadWrite)) {}
	~Mapping() { if (address != 0 && !VM::Free(address)) std::abort(); }
	const uint64_t address;
};

struct Progress final
{
	uint64_t code = 0, stack = 0, slot = 0, output = 0;
	uint64_t lock_target = reinterpret_cast<uint64_t>(&Kernel::PthreadMutexLock);
	uint64_t unlock_target = reinterpret_cast<uint64_t>(&Kernel::PthreadMutexUnlock);
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false, abort = false, guest_finished = false, guest_succeeded = false;
	bool opened_by_completed_guest = false;
	std::atomic<bool> pause_active {false};
	uint32_t lock_calls = 0, unlock_calls = 0, run_result = UINT32_MAX;
	int lock_result = -1, unlock_result = -1;
	uint64_t observed_write = 0;
	LumenAndroidFexCoreGuestStateV1 state {};
};

uint32_t QueryExecutable(void* opaque, uint64_t address, uint64_t* base,
                         uint64_t* bytes, uint32_t* writable) noexcept
{
	const auto& progress = *static_cast<Progress*>(opaque);
	if (address < progress.code || address - progress.code >= kMappingBytes) return 0;
	VM::CodeReadLease lease;
	const uint64_t page = address & ~uint64_t {4095};
	if (!VM::TryAcquireCodeReadLease(page, 4096, &lease)) return 0;
	*base = page;
	*bytes = 4096;
	*writable = 0;
	VM::ReleaseCodeReadLease(&lease);
	return 1;
}

uint32_t InvokeMutex(void* opaque, uint64_t target, uint64_t* registers,
                     uint64_t* /*next_rip*/) noexcept
{
	auto& progress = *static_cast<Progress*>(opaque);
	if (!progress.pause_active.load(std::memory_order_acquire) || registers[7] != progress.slot)
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	auto* slot = reinterpret_cast<Kernel::PthreadMutex*>(registers[7]);
	int result = -1;
	if (target == progress.lock_target && progress.lock_calls == 0 && progress.unlock_calls == 0)
	{
		++progress.lock_calls;
		result = progress.lock_result = Kernel::PthreadMutexLock(slot);
	} else if (target == progress.unlock_target && progress.lock_calls == 1 &&
	           progress.lock_result == 0 && progress.unlock_calls == 0)
	{
		// Observe the x86 store through an ordinary real VM lease while the
		// guest still owns the real native mutex. No host code writes this value.
		bool read = false;
		{
			VM::GuestDataLease lease;
			read = VM::TryAcquireGuestDataLease(progress.output, sizeof(progress.observed_write), VM::Mode::Read, &lease);
			if (read) std::memcpy(&progress.observed_write, reinterpret_cast<const void*>(progress.output),
			                      sizeof(progress.observed_write));
		}
		++progress.unlock_calls;
		result = progress.unlock_result = Kernel::PthreadMutexUnlock(slot);
		if (!read || progress.observed_write != kWrittenValue)
			return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	} else
	{
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	}
	registers[0] = static_cast<uint64_t>(static_cast<int64_t>(result));
	// The real bridge must readmit execution and perform the guest RET. Do
	// not emulate either CALL return, the data store, or the final root return.
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
}

std::vector<uint8_t> MakeProgram(const Progress& progress)
{
	std::vector<uint8_t> code;
	const auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
	const auto immediate64 = [&](uint64_t value) {
		const auto offset = code.size();
		code.resize(offset + sizeof(value));
		std::memcpy(code.data() + offset, &value, sizeof(value));
	};
	const auto call = [&](uint64_t target) {
		emit({0x48, 0xbf}); immediate64(progress.slot); // movabs rdi, mutex slot
		emit({0x48, 0xb8}); immediate64(target);        // movabs rax, native target
		emit({0xff, 0xd0});                           // call rax
	};
	emit({0x48, 0x83, 0xec, 0x08}); // sub rsp, 8: preserve SysV call alignment
	call(progress.lock_target);
	emit({0x85, 0xc0, 0x75, 0x00}); // test eax,eax; jne epilogue
	const auto lock_branch = code.size() - 1;
	emit({0x48, 0xba}); immediate64(progress.output); // movabs rdx, independent backing
	emit({0x48, 0xb8}); immediate64(kWrittenValue);   // movabs rax, value
	emit({0x48, 0x89, 0x02});                        // mov [rdx], rax (executed by FEX)
	call(progress.unlock_target);
	emit({0x85, 0xc0, 0x75, 0x00}); // test eax,eax; jne epilogue
	const auto unlock_branch = code.size() - 1;
	emit({0xb8, static_cast<uint8_t>(kGuestSuccess), static_cast<uint8_t>(kGuestSuccess >> 8), 0, 0});
	const auto epilogue = code.size();
	emit({0x48, 0x83, 0xc4, 0x08, 0xc3}); // add rsp, 8; ret to real FEX root return
	code[lock_branch] = static_cast<uint8_t>(epilogue - lock_branch - 1);
	code[unlock_branch] = static_cast<uint8_t>(epilogue - unlock_branch - 1);
	return code;
}

void PauseUntilGuestCompletes(void* opaque)
{
	auto& progress = *static_cast<Progress*>(opaque);
	std::unique_lock lock(progress.mutex);
	progress.pause_active.store(true, std::memory_order_release);
	progress.entered = true;
	progress.changed.notify_all();
	progress.changed.wait(lock, [&] { return progress.guest_finished; });
	// A failed guest may unwind the fixture but cannot satisfy its latch.
	progress.opened_by_completed_guest = progress.guest_succeeded;
	progress.pause_active.store(false, std::memory_order_release);
}

bool RunProgressGate()
{
	Mapping code, stack, slot, output;
	if (!Check(code.address && stack.address && slot.address && output.address, "managed VM fixture allocation")) return false;
	Progress progress;
	progress.code = code.address;
	progress.stack = stack.address + kMappingBytes - 8;
	progress.slot = slot.address;
	progress.output = output.address;
	const auto program = MakeProgram(progress);
	std::memcpy(reinterpret_cast<void*>(code.address), program.data(), program.size());
	std::memset(reinterpret_cast<void*>(output.address), 0, sizeof(uint64_t));
	const uint64_t root_return = LumenAndroidFexCoreRootReturnAddressV1();
	std::memcpy(reinterpret_cast<void*>(progress.stack), &root_return, sizeof(root_return));
	if (!Check(root_return != 0 && VM::Protect(code.address, kMappingBytes, VM::Mode::ExecuteRead),
	           "managed code publication and FEX root return") ||
	    !Check(LumenAndroidFexCoreRegisterHostCallTargetV1(progress.lock_target) == 1 &&
	           LumenAndroidFexCoreRegisterHostCallTargetV1(progress.unlock_target) == 1,
	           "real mutex HLE target registration")) return false;
	auto* mutex = reinterpret_cast<Kernel::PthreadMutex*>(slot.address);
	*mutex = nullptr;
	if (!Check(Kernel::PthreadMutexInit(mutex, nullptr, nullptr) == 0, "real mutex initialization")) return false;

	std::thread guest([&] {
		{
			std::unique_lock lock(progress.mutex);
			progress.changed.wait(lock, [&] { return progress.entered || progress.abort; });
			if (progress.abort) return;
		}
		progress.state.structSize = sizeof(progress.state);
		progress.state.abiVersion = 1;
		progress.state.rip = progress.code;
		progress.state.gprs[4] = progress.stack;
		progress.run_result = LumenAndroidFexCoreRunGuestV1(&progress.state, InvokeMutex, QueryExecutable, &progress);
		{
			std::lock_guard lock(progress.mutex);
			progress.guest_succeeded = progress.pause_active.load(std::memory_order_acquire) &&
			    progress.run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
			    progress.state.gprs[0] == kGuestSuccess && progress.state.gprs[4] == progress.stack + 8 &&
			    progress.state.faultSignal == 0 && progress.lock_calls == 1 && progress.unlock_calls == 1 &&
			    progress.lock_result == 0 && progress.unlock_result == 0 && progress.observed_write == kWrittenValue;
			progress.guest_finished = true;
		}
		progress.changed.notify_all();
	});

	// This hook owns a real GpuHandoffClaimScope and calls the same typed
	// no-flush release/reacquire operation as production owned preparation.
	const bool resumed = Graphics::GraphicsRunOwnedPreparationPauseTest(PauseUntilGuestCompletes, &progress);
	{
		std::lock_guard lock(progress.mutex);
		if (!progress.entered) progress.abort = true;
	}
	progress.changed.notify_all();
	guest.join();
	const bool destroyed = Kernel::PthreadMutexDestroy(mutex) == 0;
	if (!resumed || !progress.opened_by_completed_guest || !destroyed)
		std::fprintf(stderr, "OwnedDrawPreparationFexTest: resumed=%u entered=%u guest_completed=%u runtime=%u"
		    " lock_calls=%u unlock_calls=%u lock_result=%d unlock_result=%d destroyed=%u\n",
		    resumed, progress.entered, progress.opened_by_completed_guest, progress.run_result,
		    progress.lock_calls, progress.unlock_calls, progress.lock_result, progress.unlock_result, destroyed);
	return Check(resumed && progress.opened_by_completed_guest && destroyed,
	             "FEX lock/write/unlock/root-return must open the paused worker's latch before exclusive reacquisition");
}

} // namespace

static bool OwnedMutationTokenGate()
{
	if (!Check(VM::GetOwnedGuestMemoryMutationToken() == 0, "no adopted owner has no metadata token")) return false;
	Mapping backing;
	if (!Check(backing.address != 0, "metadata fixture allocation")) return false;
	const bool cache_selected = std::getenv("LUMEN_GPU_BACKING_QUERY_CACHE") != nullptr &&
	    std::strcmp(std::getenv("LUMEN_GPU_BACKING_QUERY_CACHE"), "1") == 0;
	if (cache_selected && !Graphics::GpuMemoryBackingQueryCacheContractTest(backing.address, kMappingBytes, false)) return false;
	uint64_t previous = 0;
	uint64_t previous_scope = 0;
	for (unsigned iteration = 0; iteration != 2; ++iteration)
	{
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		if (!Check(token != 0 && token != previous, "exclusive tokens must distinguish reacquisition")) return false;
		bool valid = VM::GetOwnedGuestMemoryMutationToken() == 0;
		{
			VM::GuestMemoryMutationOwnerScope owner(token);
			const auto scope = VM::GetOwnedGuestMemoryMutationToken();
			valid &= owner.IsActive() && scope != 0 && scope != previous_scope;
			previous_scope = scope;
			if (cache_selected) valid &= Graphics::GpuMemoryBackingQueryCacheContractTest(backing.address, kMappingBytes, true);
			valid &= Graphics::GpuMemoryRangeClaimPlanContractTest(backing.address);
			bool foreign_denied = false;
			std::thread other([&] { foreign_denied = VM::GetOwnedGuestMemoryMutationToken() == 0; });
			other.join();
			valid &= foreign_denied;
			VM::GuestMemoryMutationOwnerScope nested(token);
			valid &= !nested.IsActive() && VM::GetOwnedGuestMemoryMutationToken() == scope;
		}
		valid &= VM::GetOwnedGuestMemoryMutationToken() == 0;
		if (cache_selected) valid &= Graphics::GpuMemoryBackingQueryCacheContractTest(backing.address, kMappingBytes, false);
		valid &= LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
		valid &= VM::GetOwnedGuestMemoryMutationToken() == 0;
		if (!Check(valid, "metadata token is exact, thread-local and bounded by adopted owner")) return false;
		previous = token;
	}
	std::puts("OwnedMutationTokenTest: PASS unadopted/nested/foreign/ended=denied reacquired=distinct");
	return true;
}

static bool NativeOwnerDowngradeGate()
{
	namespace Cpu = Lumen::GuestCpu;
	Mapping backing, outside, code, stack;
	if (!Check(backing.address && outside.address && code.address && stack.address, "native owner mappings")) return false;
	Progress progress;
	progress.code = code.address;
	progress.stack = stack.address + kMappingBytes - 8;
	const uint8_t program[] = {0xb8, 0x5a, 0x17, 0, 0, 0xc3}; // mov eax,175ah; ret
	std::memcpy(reinterpret_cast<void*>(code.address), program, sizeof(program));
	const uint64_t root_return = LumenAndroidFexCoreRootReturnAddressV1();
	std::memcpy(reinterpret_cast<void*>(progress.stack), &root_return, sizeof(root_return));
	if (!Check(root_return != 0 && VM::Protect(code.address, kMappingBytes, VM::Mode::ExecuteRead), "native gate executable")) return false;
	const uint64_t exclusive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
	if (!Check(exclusive != 0, "native gate exclusive admission")) return false;
	const auto owner_id = VM::CreateGuestBackingHandoffOwner();
	bool okay = false;
	{
		VM::GuestMemoryMutationOwnerScope owner(exclusive);
		okay = owner.IsActive() && owner_id != 0 && VM::MarkGuestBackingHandoffRangeOwned(owner_id, backing.address, 64) &&
		    Cpu::PublishMemoryHandoffNativeClaim(owner_id);
	}
	if (!okay)
	{
		(void)VM::ReleaseGuestBackingHandoffOwner(owner_id);
		(void)LumenAndroidFexCoreEndGuestMemoryMutationV2(exclusive, 0, 0, 0, nullptr, nullptr);
		return Check(false, "native claim enrollment");
	}
	const auto pin = LumenAndroidFexCoreDowngradeExclusiveToNativeGuestDataLeaseV1(exclusive);
	if (pin == 0)
	{
		(void)Cpu::RetireMemoryHandoffNativeClaim(owner_id);
		(void)VM::ReleaseGuestBackingHandoffOwner(owner_id);
		(void)LumenAndroidFexCoreEndGuestMemoryMutationV2(exclusive, 0, 0, 0, nullptr, nullptr);
		return Check(false, "native atomic downgrade");
	}
	{
		Cpu::MemoryHandoffNativeClaimScope claim(owner_id);
		VM::GuestNativeMemoryOwnerScope native(pin);
		okay &= claim.IsActive() && native.IsActive() && VM::GetOwnedGuestMemoryMutationToken() == 0;
		VM::GuestNativeMemoryOwnerScope nested(pin);
		okay &= !nested.IsActive();
		bool foreign_denied = false;
		std::thread foreign([&] {
			VM::GuestNativeMemoryOwnerScope other(pin);
			foreign_denied = !other.IsActive() && LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(pin) == 0;
		});
		foreign.join();
		okay &= foreign_denied;
		{
			Cpu::MemoryHandoffBypass producer;
			VM::GuestDataLease first, second, denied;
			okay &= VM::TryAcquireGuestDataLease(backing.address, 8, VM::Mode::ReadWrite, &first, false);
			okay &= VM::TryAcquireGuestDataLease(backing.address + 8, 8, VM::Mode::Read, &second, false);
			okay &= !VM::TryAcquireGuestDataLease(backing.address + 63, 2, VM::Mode::Read, &denied, false) &&
			    !VM::TryAcquireGuestDataLease(outside.address, 8, VM::Mode::Read, &denied, false) &&
			    !VM::CurrentNativeClaimCoversGuestRange(backing.address + 64, 1);
			uint64_t value = 0;
			okay &= !VM::CopyGuestBackingDataForGpu(outside.address, &value, sizeof(value), false) &&
			    !VM::MarkGuestBackingHandoffRangeOwned(owner_id, outside.address, 8);
			okay &= !VM::Protect(backing.address, kMappingBytes, VM::Mode::Read);
		}
		okay &= !VM::CurrentThreadHasMemoryLease() && LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(pin) == 1;
		// Real FEX executes to its ordinary root return while this worker still
		// owns the native mapping pin and its exact physical byte claim.
		std::thread guest([&] {
			progress.state.structSize = sizeof(progress.state);
			progress.state.abiVersion = 1;
			progress.state.rip = progress.code;
			progress.state.gprs[4] = progress.stack;
			progress.run_result = LumenAndroidFexCoreRunGuestV1(&progress.state, InvokeMutex, QueryExecutable, &progress);
		});
		guest.join();
		okay &= progress.run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
		    progress.state.gprs[0] == kGuestSuccess && progress.state.gprs[4] == progress.stack + 8 &&
		    progress.state.faultSignal == 0 && LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(pin) == 1;
		okay &= Cpu::RetireMemoryHandoffNativeClaim(owner_id);
		VM::GuestDataLease retired;
		okay &= !VM::TryAcquireGuestDataLease(backing.address, 8, VM::Mode::Read, &retired, false);
	}
	okay &= LumenAndroidFexCoreEndNativeGuestDataLeaseV1(pin) == 1;
	const auto cleanup = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
	if (cleanup != 0)
	{
		okay &= VM::ReleaseGuestBackingHandoffOwner(owner_id);
		okay &= LumenAndroidFexCoreEndGuestMemoryMutationV2(cleanup, 0, 0, 0, nullptr, nullptr) == 1;
	} else okay = false;
	if (okay) std::puts("NativeOwnerDowngradeTest: PASS exact-coverage nested-borrows affinity no-mutation pin-retained real-FEX-progress");
	return Check(okay, "native mapping owner and real FEX progress");
}

// The witness is a waiter registered by the actual JIT handoff callback. No
// timeout or scheduler delay establishes that the claimed instruction stopped.
struct RangeProgress final
{
	Progress guest;
	uint64_t claimed = 0, outside = 0;
	bool completed_under_registry = false;
	bool observed_waiter = false, disjoint_visible = false, claimed_unchanged = false;
	bool native_context = false;
};

static void ObserveClaimedGuestStore(void* opaque)
{
	namespace Cpu = Lumen::GuestCpu;
	auto& progress = *static_cast<RangeProgress*>(opaque);
	progress.native_context = Cpu::CurrentMemoryHandoffNativeClaimOwner() != 0 &&
	    VM::GetOwnedGuestMemoryMutationToken() == 0 && !VM::CurrentThreadHasMemoryLease();
	// Keep code, stack and the first store out of the conservative page bucket.
	// If fixture addresses collide, fail without interpreting that as progress.
	progress.native_context &= !Cpu::HandoffDetail::NativeClaimBucketConflict(progress.outside, 8) &&
	    !Cpu::HandoffDetail::NativeClaimBucketConflict(progress.guest.code, kMappingBytes) &&
	    !Cpu::HandoffDetail::NativeClaimBucketConflict(progress.guest.stack, 8);
	{
		std::lock_guard lock(progress.guest.mutex);
		progress.guest.abort = !progress.native_context;
		progress.guest.entered = progress.native_context;
	}
	progress.guest.changed.notify_all();
	if (!progress.native_context) return;
	auto& registry = Cpu::HandoffDetail::GetRegistry();
	{
		std::unique_lock lock(registry.mutex);
		registry.native_claim_ready.wait(lock, [&] {
			return registry.native_claim_waiters != 0 || progress.completed_under_registry;
		});
		progress.observed_waiter = registry.native_claim_waiters != 0 && !progress.completed_under_registry;
	}
	if (!progress.observed_waiter) return;
	{
		VM::GuestDataLease lease;
		uint64_t value = UINT64_MAX;
		if (VM::TryAcquireGuestDataLease(progress.claimed, sizeof(value), VM::Mode::Read, &lease))
		{
			std::memcpy(&value, reinterpret_cast<const void*>(progress.claimed), sizeof(value));
			progress.claimed_unchanged = value == 0;
		}
	}
	// The native owner cannot borrow outside its claim. An ordinary independent
	// Core reader observes the disjoint store through the production VM path.
	std::thread observer([&] {
		VM::GuestDataLease lease;
		uint64_t value = 0;
		if (VM::TryAcquireGuestDataLease(progress.outside, sizeof(value), VM::Mode::Read, &lease))
		{
			std::memcpy(&value, reinterpret_cast<const void*>(progress.outside), sizeof(value));
			progress.disjoint_visible = value == kWrittenValue;
		}
	});
	observer.join();
}

static bool NativeRangeExecutionGate()
{
	const char* selected = std::getenv("LUMEN_GPU_RANGE_EXECUTION");
	if (selected == nullptr || std::strcmp(selected, "1") != 0) return true;
	Mapping claimed, outside, code, stack;
	if (!Check(claimed.address && outside.address && code.address && stack.address, "range execution mappings")) return false;
	RangeProgress progress;
	progress.claimed = claimed.address;
	progress.outside = outside.address;
	progress.guest.code = code.address;
	progress.guest.stack = stack.address + kMappingBytes - 8;
	std::vector<uint8_t> program;
	const auto immediate = [&](uint64_t value) {
		const auto at = program.size();
		program.resize(at + sizeof(value));
		std::memcpy(program.data() + at, &value, sizeof(value));
	};
	program.insert(program.end(), {0x48, 0xb8}); immediate(kWrittenValue); // movabs rax,value
	program.insert(program.end(), {0x48, 0xba}); immediate(outside.address); // movabs rdx,outside
	program.insert(program.end(), {0x48, 0x89, 0x02}); // mov [rdx],rax: disjoint progress
	program.insert(program.end(), {0x48, 0xba}); immediate(claimed.address);
	program.insert(program.end(), {0x48, 0x89, 0x02}); // mov [rdx],rax: must await claim retirement
	program.insert(program.end(), {0xb8, 0x5a, 0x17, 0, 0, 0xc3}); // mov eax,175ah; ret
	std::memcpy(reinterpret_cast<void*>(code.address), program.data(), program.size());
	const uint64_t zero = 0, root_return = LumenAndroidFexCoreRootReturnAddressV1();
	std::memcpy(reinterpret_cast<void*>(claimed.address), &zero, sizeof(zero));
	std::memcpy(reinterpret_cast<void*>(outside.address), &zero, sizeof(zero));
	std::memcpy(reinterpret_cast<void*>(progress.guest.stack), &root_return, sizeof(root_return));
	if (!Check(root_return != 0 && VM::Protect(code.address, kMappingBytes, VM::Mode::ExecuteRead), "range execution code")) return false;
	std::thread guest([&] {
		{
			std::unique_lock lock(progress.guest.mutex);
			progress.guest.changed.wait(lock, [&] { return progress.guest.entered || progress.guest.abort; });
			if (progress.guest.abort) return;
		}
		auto& state = progress.guest.state;
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = code.address;
		state.gprs[4] = progress.guest.stack;
		progress.guest.run_result = LumenAndroidFexCoreRunGuestV1(&state, InvokeMutex, QueryExecutable, &progress.guest);
		auto& registry = Lumen::GuestCpu::HandoffDetail::GetRegistry();
		{
			std::lock_guard lock(registry.mutex);
			progress.completed_under_registry = true;
		}
		registry.native_claim_ready.notify_all();
	});
	const bool resumed = Graphics::GraphicsRunNativeRangePhaseTest(claimed.address, 64, ObserveClaimedGuestStore, &progress);
	{
		std::lock_guard lock(progress.guest.mutex);
		if (!progress.guest.entered) progress.guest.abort = true;
	}
	progress.guest.changed.notify_all();
	guest.join();
	uint64_t final_value = 0;
	bool final_read = false;
	{
		VM::GuestDataLease lease;
		final_read = VM::TryAcquireGuestDataLease(claimed.address, sizeof(final_value), VM::Mode::Read, &lease);
		if (final_read) std::memcpy(&final_value, reinterpret_cast<const void*>(claimed.address), sizeof(final_value));
	}
	const auto& state = progress.guest.state;
	const bool okay = resumed && progress.native_context && progress.observed_waiter && progress.disjoint_visible &&
	    progress.claimed_unchanged && progress.completed_under_registry && final_read && final_value == kWrittenValue &&
	    progress.guest.run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && state.gprs[0] == kGuestSuccess &&
	    state.gprs[4] == progress.guest.stack + 8 && state.faultSignal == 0;
	if (okay) std::puts("NativeRangeExecutionTest: PASS runtime=FEX disjoint-store=visible claimed-store=registered-waiter"
	    " claimed-before-retire=unchanged claimed-after-retire=written epoch=reacquired native-wait=retains-pin");
	return Check(okay, "production range phase permits disjoint FEX store and blocks overlapping FEX store until retirement");
}

// Invoked by lumen_graphics_state_test in its own process, after that
// executable's ordinary Threads/Config/Log initialization and runtime probe.
int GraphicsOwnedPreparationFexTest()
{
	DeadlockWatchdog watchdog;
	if (!Check(Lumen::GuestCpu::Select(Lumen::GuestCpu::Provider::Fex) == Lumen::GuestCpu::SelectionResult::Selected,
	           "FEX provider selection")) return 1;
	VM::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	if (!Check(LumenAndroidFexCoreRuntimeProbeV1(nullptr) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1,
	           "real FEX runtime readiness")) return 1;
	if (!NativeOwnerDowngradeGate()) return 1;
	// These initialize host bookkeeping only. No window, Vulkan device,
	// command submission or synthetic memory-permission/handoff provider.
	// The production tracker creates its diagnostic database unconditionally.
	// This minimal executable initializes neither SQLite nor the Math RNG used
	// by its custom VFS randomness callback when the first journal is created.
	Lumen::Math::Rand::Init();
	Lumen::Core::Database::Init();
	Graphics::GraphicsRenderInit();
	Graphics::GpuMemoryInit();
	if (!OwnedMutationTokenGate()) return 1;
	if (!Check(Graphics::GpuMemoryCpuHandoffEnabled(), "production renderer handoff registration") ||
	    !RunProgressGate() || !NativeRangeExecutionGate()) return 1;
	const char* range_selected = std::getenv("LUMEN_GPU_RANGE_EXECUTION");
	if (range_selected != nullptr && std::strcmp(range_selected, "1") == 0)
	{
		Mapping claimed, dirty;
		if (!Check(claimed.address != 0 && dirty.address != 0 &&
		    Graphics::GpuMemoryNativeRangeDirtyHandoffContractTest(claimed.address, dirty.address),
		    "native draw and unrelated CPU dirty publication use compatible lock order")) return 1;
		std::puts("NativeRangeExecutionSuite: PASS real-FEX-stores native-claim CPU-dirty-publication production-lock-order");
	}
	std::puts("OwnedDrawPreparationFexTest: PASS runtime=FEX vm=managed mutex=real"
	          " guest=lock-write-unlock-root-return latch=guest-completion epoch=reacquired draw=not-exercised");
	return 0;
}
