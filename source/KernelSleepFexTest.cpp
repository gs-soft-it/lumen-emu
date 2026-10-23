#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include "AndroidFexCoreRuntime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

namespace VM = Lumen::Core::VirtualMemory;
namespace Kernel = Lumen::Libs::LibKernel;
constexpr uint64_t kHostPage = 16384;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "KernelSleepFexTest: %s\n", message);
	return condition;
}

bool TestSleepBinding()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel registration failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.name                 = "-ZR+hG7aDHw";
	exact.library              = "libkernel";
	exact.library_version      = 1;
	exact.module               = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type                 = Lumen::Loader::SymbolType::Func;
	const auto* binding        = symbols.Find(exact);
	if (!Check(binding != nullptr && binding->vaddr == reinterpret_cast<uint64_t>(&Kernel::KernelSleep),
	           "exact sceKernelSleep binding differs"))
	{
		return false;
	}
	exact.library_version = 0;
	if (!Check(symbols.Find(exact) == nullptr && symbols.FindByStdName(U"sceKernelSleep") == nullptr,
	           "sceKernelSleep gained a mismatched or standard-name fallback"))
	{
		return false;
	}

	Lumen::Loader::SymbolResolve exception {};
	exception.name                 = "WkwEd3N7w0Y";
	exception.library              = "libkernel_unity";
	exception.library_version      = 1;
	exception.module               = "libkernel";
	exception.module_version_major = 1;
	exception.module_version_minor = 1;
	exception.type                 = Lumen::Loader::SymbolType::Func;
	const auto* exception_binding  = symbols.Find(exception);
	if (!Check(exception_binding != nullptr, "exact sceKernelInstallExceptionHandler binding is absent")) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(exception_binding->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 2, "exception-handler scalar ABI differs")) return false;
	using InstallExceptionHandler = int32_t(LUMEN_SYSV_ABI*)(int32_t, uint64_t);
	const auto install = reinterpret_cast<InstallExceptionHandler>(exception_binding->vaddr);
	exception.name                    = "il03nluKfMk";
	const auto* raise_exception       = symbols.Find(exception);
	const auto* raise_metadata        = raise_exception == nullptr ? nullptr : symbols.HostCallScalarMetadataFor(raise_exception->vaddr);
	if (!Check(raise_exception != nullptr && raise_metadata != nullptr && raise_metadata->arity == 2,
	           "exact exception-raise binding or ABI differs"))
	{
		return false;
	}
	using RaiseException = int32_t(LUMEN_SYSV_ABI*)(uint64_t, int32_t);
	const auto raise = reinterpret_cast<RaiseException>(raise_exception->vaddr);
	constexpr uint64_t handler = 0x0000007193194270;
	// Deliberately not a live pool or published-self handle: the raise must fail
	// closed instead of dereferencing an unvalidated guest value.
	constexpr uint64_t target = 0x0000007c7f5a3000;
	if (!Check(raise(target, 30) == Kernel::KERNEL_ERROR_EINVAL, "exception raise succeeded without an installed handler") ||
	    !Check(install(7, handler) == Kernel::KERNEL_ERROR_EINVAL, "disallowed exception signal was accepted") ||
	    !Check(install(30, handler) == 0, "ready FEX exception route was not acknowledged") ||
	    !Check(install(30, handler) == Kernel::KERNEL_ERROR_EAGAIN, "second handler for one signal was accepted") ||
	    !Check(raise(target, 29) == Kernel::KERNEL_ERROR_EINVAL, "raise accepted a signal other than SIGUSR1") ||
	    !Check(raise(target, 30) == Kernel::KERNEL_ERROR_ESRCH,
	           "raise accepted an unknown or stale pthread handle"))
	{
		return false;
	}
	exception.library = "libkernel";
	return Check(symbols.Find(exception) == nullptr && symbols.FindByStdName(U"sceKernelInstallExceptionHandler") == nullptr &&
	                 symbols.FindByStdName(U"sceKernelRaiseException") == nullptr,
	             "exception handler escaped its exact unity namespace");
}

struct FexMapping final
{
	FexMapping(): address(VM::Alloc(0, kHostPage, VM::Mode::ReadWrite)) {}
	~FexMapping() { if (address != 0 && !VM::Free(address)) std::abort(); }
	const uint64_t address;
};

enum class SleepKind { Micro, Seconds, Nano, Mutex, TimedMutex };

struct SleepContext final
{
	SleepKind kind;
	uint64_t code;
	uint64_t request;
	uint64_t remaining;
	std::atomic_bool started {false};
	std::atomic_bool finished {false};
	int result = Kernel::KERNEL_ERROR_EBUSY;
	uint32_t run_result = UINT32_MAX;
};

void FexSleepTarget() {}

uint32_t FexSleepQuery(void* opaque, uint64_t address, uint64_t* base,
                       uint64_t* size, uint32_t* writable) noexcept
{
	const auto& context = *static_cast<SleepContext*>(opaque);
	if (address < context.code || address - context.code >= kHostPage) return 0;
	VM::CodeReadLease lease;
	const uint64_t page = address & ~uint64_t {4095};
	if (!VM::TryAcquireCodeReadLease(page, 4096, &lease)) return 0;
	*base = page;
	*size = 4096;
	*writable = 0;
	VM::ReleaseCodeReadLease(&lease);
	return 1;
}

uint32_t FexSleepCallback(void* opaque, uint64_t target, uint64_t* gprs,
                          uint64_t* next_rip) noexcept
{
	auto& context = *static_cast<SleepContext*>(opaque);
	if (target != reinterpret_cast<uint64_t>(&FexSleepTarget))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	context.started.store(true, std::memory_order_release);
	switch (context.kind)
	{
		case SleepKind::Micro: context.result = Kernel::KernelUsleep(3000000); break;
		case SleepKind::Seconds: context.result = static_cast<int>(Kernel::KernelSleep(1)); break;
		case SleepKind::Nano:
			context.result = Kernel::KernelNanosleep(
			    reinterpret_cast<const Kernel::KernelTimespec*>(context.request),
			    reinterpret_cast<Kernel::KernelTimespec*>(context.remaining));
			break;
		case SleepKind::Mutex:
		case SleepKind::TimedMutex:
		{
			auto* mutex = reinterpret_cast<Kernel::PthreadMutex*>(context.request);
			context.result = context.kind == SleepKind::Mutex
			    ? Kernel::PthreadMutexLock(mutex)
			    : Kernel::PthreadMutexTimedlock(mutex, 3000000);
			if (context.result == 0) context.result = Kernel::PthreadMutexUnlock(mutex);
			break;
		}
	}
	gprs[0] = static_cast<uint64_t>(static_cast<int64_t>(context.result));
	std::memcpy(next_rip, reinterpret_cast<const void*>(gprs[4]), sizeof(*next_rip));
	gprs[4] += sizeof(*next_rip);
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool RunSleepCase(SleepKind kind, bool invalidate_output)
{
	FexMapping code, stack, request, output, mutation;
	if (!Check(code.address && stack.address && request.address && output.address && mutation.address,
	           "fixture allocation failed")) return false;
	const uint64_t target = reinterpret_cast<uint64_t>(&FexSleepTarget);
	std::array<uint8_t, 13> program {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
	std::memcpy(program.data() + 2, &target, sizeof(target));
	std::memcpy(reinterpret_cast<void*>(code.address), program.data(), program.size());
	if (!Check(VM::Protect(code.address, kHostPage, VM::Mode::ExecuteRead),
	           "fixture code publication failed")) return false;
	auto* requested = reinterpret_cast<Kernel::KernelTimespec*>(request.address);
	auto* remaining = reinterpret_cast<Kernel::KernelTimespec*>(output.address);
	*requested = {1, 0};
	*remaining = {INT64_MIN, INT64_MIN};
	const bool mutex_case = kind == SleepKind::Mutex || kind == SleepKind::TimedMutex;
	auto* mutex = reinterpret_cast<Kernel::PthreadMutex*>(request.address);
	if (mutex_case)
	{
		*mutex = nullptr;
		if (!Check(Kernel::PthreadMutexInit(mutex, nullptr, nullptr) == 0 &&
		           Kernel::PthreadMutexLock(mutex) == 0, "fixture mutex ownership failed")) return false;
	}
	SleepContext context {kind, code.address, request.address, output.address};
	const auto run_start = std::chrono::steady_clock::now();
	std::thread waiter([&] {
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = code.address;
		state.gprs[4] = stack.address + kHostPage - 8;
		const uint64_t root_return = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
		context.run_result = LumenAndroidFexCoreRunGuestV1(&state, FexSleepCallback,
		                                                   FexSleepQuery, &context);
		context.finished.store(true, std::memory_order_release);
	});
	const auto deadline = run_start + std::chrono::seconds(4);
	while (!context.started.load(std::memory_order_acquire) &&
	       !context.finished.load(std::memory_order_acquire) &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool started = context.started.load(std::memory_order_acquire);
	if (started) std::this_thread::sleep_for(std::chrono::milliseconds(40));
	const auto mutation_start = std::chrono::steady_clock::now();
	const bool mutated = started && VM::Protect(mutation.address, kHostPage, VM::Mode::Read);
	const auto mutation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - mutation_start).count();
	const bool still_sleeping = !context.finished.load(std::memory_order_acquire);
	// The contender must yield its FEX reader while the owner still holds the
	// mutex. Release only after the exclusive mutation has completed or failed.
	const bool owner_released = !mutex_case || Kernel::PthreadMutexUnlock(mutex) == 0;
	if (kind == SleepKind::Nano && mutated)
	{
		// The request is guest-owned but the sleep duration must remain the
		// input captured before the HLE reader was yielded.
		*requested = {0, 0};
		if (invalidate_output) VM::Protect(output.address, kHostPage, VM::Mode::Read);
	}
	waiter.join();
	const bool mutex_destroyed = !mutex_case || Kernel::PthreadMutexDestroy(mutex) == 0;
	const auto run_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - run_start).count();
	const int expected_result = invalidate_output ? Kernel::KERNEL_ERROR_EFAULT : 0;
	const auto mutation_budget_ms = kind == SleepKind::Micro ? 1500 : 700;
	const bool ok = started && mutated && mutation_ms < mutation_budget_ms && still_sleeping &&
	             (mutex_case || run_ms >= 700) && owner_released && mutex_destroyed &&
	             context.run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	             context.result == expected_result &&
	             (kind != SleepKind::Nano || invalidate_output || remaining->tv_sec != INT64_MIN);
	if (!ok)
		std::fprintf(stderr, "KernelSleepFexTest: kind=%d invalid_output=%d mutation_ms=%lld run_ms=%lld result=%d\n",
		             static_cast<int>(kind), invalidate_output, static_cast<long long>(mutation_ms),
		             static_cast<long long>(run_ms), context.result);
	return Check(ok, "native wait retained the FEX reader or lost its guest input/output contract");
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	if (!TestSleepBinding()) return 1;
	if (!Check(LumenAndroidFexCoreRegisterHostCallTargetV1(
	               reinterpret_cast<uint64_t>(&FexSleepTarget)) == 1,
	           "fixture host-call registration failed")) return 1;
	if (!RunSleepCase(SleepKind::Micro, false) ||
	    !RunSleepCase(SleepKind::Seconds, false) ||
	    !RunSleepCase(SleepKind::Nano, false) ||
	    !RunSleepCase(SleepKind::Nano, true) ||
	    !RunSleepCase(SleepKind::Mutex, false) ||
	    !RunSleepCase(SleepKind::TimedMutex, false)) return 1;
	std::puts("KernelSleepFexTest: PASS");
	return 0;
}
