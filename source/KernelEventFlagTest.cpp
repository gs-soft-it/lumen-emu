#include "Lumen/Core/Threads.h"
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
#include "Lumen/Core/VirtualMemory.h"
#include "AndroidFexCoreRuntime.h"
#endif

#include "Emulator/Config.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace FlagApi = Kernel::EventFlag;
using ClearEventFlagFunction = decltype(&FlagApi::KernelClearEventFlag);

class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fputs("KernelEventFlagTest: timed out\n", stderr);
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}
	~Watchdog()
	{
		{
			std::lock_guard lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_done = false;
	std::thread m_thread;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "KernelEventFlagTest: %s\n", message);
	}
	return condition;
}

bool TestExport(Lumen::Loader::SymbolDatabase* symbols, ClearEventFlagFunction* clear,
                const Lumen::Loader::HostCallScalarMetadata** cancel)
{
	if (!Check(Lumen::Libs::Init(U"libkernel_1", symbols), "libkernel registration failed")) return false;
	Lumen::Loader::SymbolResolve query {};
	query.name = "8mql9OcQnd4";
	query.library = "libkernel";
	query.library_version = 1;
	query.module = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	const auto* symbol = symbols->Find(query);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&FlagApi::KernelDeleteEventFlag),
	           "delete export has the wrong binding")) return false;
	for (unsigned mismatch = 0; mismatch < 5; ++mismatch)
	{
		auto wrong = query;
		switch (mismatch)
		{
			case 0: wrong.library = "unrelated_library"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
		}
		if (!Check(symbols->Find(wrong) == nullptr, "delete export matched the wrong namespace or version")) return false;
	}
	query.name = "7uhBFWRAS60";
	symbol = symbols->Find(query);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&FlagApi::KernelClearEventFlag),
	           "clear export is missing or has the wrong binding")) return false;
	for (unsigned mismatch = 0; mismatch < 5; ++mismatch)
	{
		auto wrong = query;
		switch (mismatch)
		{
			case 0: wrong.library = "unrelated_library"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
		}
		if (!Check(symbols->Find(wrong) == nullptr, "clear export matched the wrong namespace or version")) return false;
	}
	*clear = reinterpret_cast<ClearEventFlagFunction>(symbol->vaddr);
	query.name = "PZku4ZrXJqg";
	symbol = symbols->Find(query);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&FlagApi::KernelCancelEventFlag),
	           "cancel export is missing or has the wrong binding")) return false;
	for (unsigned mismatch = 0; mismatch < 5; ++mismatch)
	{
		auto wrong = query;
		switch (mismatch)
		{
			case 0: wrong.library = "unrelated_library"; break;
			case 1: wrong.module = "unrelated_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
		}
		if (!Check(symbols->Find(wrong) == nullptr, "cancel export matched the wrong namespace or version")) return false;
	}
	*cancel = symbols->HostCallScalarMetadataFor(symbol->vaddr);
	using ScalarType = Lumen::Loader::HostCallScalarType;
	if (!Check(*cancel != nullptr && (*cancel)->arity == 3 && (*cancel)->result == ScalarType::Gp &&
	           (*cancel)->arguments[0] == ScalarType::Gp && (*cancel)->arguments[1] == ScalarType::Gp &&
	           (*cancel)->arguments[2] == ScalarType::Gp && (*cancel)->invoker != nullptr,
	           "cancel export scalar ABI metadata differs")) return false;
	return true;
}

bool TestClearThroughExport(ClearEventFlagFunction clear)
{
	if (!Check(clear != nullptr, "clear behavior test has no resolved export") ||
	    !Check(clear(nullptr, UINT64_MAX) == Kernel::KERNEL_ERROR_ESRCH, "resolved null clear did not return ESRCH")) return false;
	constexpr uint64_t high_bit = uint64_t {1} << 63;
	constexpr uint64_t initial_bits = high_bit | 0xa5;
	constexpr uint64_t retained_bits = initial_bits & ~uint64_t {1};
	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-clear", 0x10, initial_bits, nullptr) == 0 && flag != nullptr,
	           "clear fixture creation failed")) return false;

	uint64_t result_bits = 0;
	bool ok = Check(clear(flag, UINT64_MAX) == 0 &&
	                FlagApi::KernelPollEventFlag(flag, initial_bits, 1, &result_bits) == 0 && result_bits == initial_bits,
	                "clear with all-one mask did not preserve the complete 64-bit pattern");
	ok = Check(clear(flag, ~uint64_t {1}) == 0 &&
	           FlagApi::KernelPollEventFlag(flag, retained_bits, 1, &result_bits) == 0 && result_bits == retained_bits &&
	           FlagApi::KernelPollEventFlag(flag, 1, 1, &result_bits) == Kernel::KERNEL_ERROR_EBUSY && result_bits == retained_bits,
	           "clear with complemented bit zero did not remove only bit zero") && ok;
	ok = Check(clear(flag, 0) == 0 &&
	           FlagApi::KernelPollEventFlag(flag, initial_bits, 2, &result_bits) == Kernel::KERNEL_ERROR_EBUSY && result_bits == 0,
	           "clear with zero mask did not remove every bit") && ok;

	// Poll and a zero-timeout wait keep this serial lifecycle test bounded.
	// Set restores both low and high bits; Wait consumes only the requested bit.
	Kernel::KernelUseconds timeout = 0;
	ok = Check(FlagApi::KernelSetEventFlag(flag, high_bit | 1) == 0 &&
	           FlagApi::KernelWaitEventFlag(flag, 1, 0x21, &result_bits, &timeout) == 0 && result_bits == (high_bit | 1) &&
	           FlagApi::KernelPollEventFlag(flag, high_bit, 1, &result_bits) == 0 && result_bits == high_bit &&
	           FlagApi::KernelPollEventFlag(flag, 1, 1, &result_bits) == Kernel::KERNEL_ERROR_EBUSY && result_bits == high_bit,
	           "set/wait/poll lifecycle changed after resolved clear") && ok;
	ok = Check(clear(flag, 0) == 0 &&
	           FlagApi::KernelPollEventFlag(flag, high_bit, 1, &result_bits) == Kernel::KERNEL_ERROR_EBUSY && result_bits == 0,
	           "resolved clear failed after set/wait/poll reuse") && ok;
	const bool deleted = Check(FlagApi::KernelDeleteEventFlag(flag) == 0 && FlagApi::EventFlagRegistryEmptyForTest(),
	                           "clear fixture deletion retained a registry entry");
	return deleted && ok;
}

bool TestCancelThroughExport(const Lumen::Loader::HostCallScalarMetadata& cancel)
{
	constexpr uint64_t first_pattern = UINT64_C(0x8000000080000020);
	constexpr uint64_t second_pattern = UINT64_C(0x4000000000000004);
	constexpr uint32_t count_canary = 0xa65c9e31u;
	std::array<uint32_t, 3> counts {count_canary, count_canary, count_canary};
	const std::array<uint64_t, 3> null_arguments {
	    0, first_pattern, reinterpret_cast<uint64_t>(&counts[1]),
	};
	if (!Check(cancel.invoker(null_arguments.data()) == static_cast<uint32_t>(Kernel::KERNEL_ERROR_ESRCH) &&
	           counts == std::array<uint32_t, 3> {count_canary, count_canary, count_canary},
	           "null cancel handle did not preserve ESRCH and output")) return false;

	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-cancel", 0x10, 0, nullptr) == 0 && flag != nullptr,
	           "cancel fixture creation failed")) return false;
	int wait_result = 0;
	uint64_t result_bits = 0;
	std::atomic_bool finished {false};
	std::thread waiter([flag, &wait_result, &result_bits, &finished] {
		wait_result = FlagApi::KernelWaitEventFlag(flag, 1, 1, &result_bits, nullptr);
		finished.store(true, std::memory_order_release);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool parked = false;
	while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		if (FlagApi::EventFlagHasWaitersForTest(flag))
		{
			parked = true;
			break;
		}
		std::this_thread::yield();
	}
	if (!parked)
	{
		FlagApi::KernelSetEventFlag(flag, 1);
		waiter.join();
		FlagApi::KernelDeleteEventFlag(flag);
		return Check(false, "cancel waiter never reached the protected suspended state");
	}

	std::array<uint64_t, 3> arguments {
	    reinterpret_cast<uint64_t>(flag), first_pattern, reinterpret_cast<uint64_t>(&counts[1]),
	};
	const uint64_t first_status = cancel.invoker(arguments.data());
	if (first_status != 0) FlagApi::KernelSetEventFlag(flag, 1);
	waiter.join();
	bool ok = Check(first_status == 0 && wait_result == Kernel::KERNEL_ERROR_ECANCELED &&
	                result_bits == first_pattern && counts[0] == count_canary && counts[1] == 1 &&
	                counts[2] == count_canary,
	                "cancel did not release its waiter with ECANCELED, full pattern and count one");

	counts[1] = count_canary;
	arguments[1] = second_pattern;
	const uint64_t second_status = cancel.invoker(arguments.data());
	uint64_t observed_bits = 0;
	ok = Check(second_status == 0 && counts[0] == count_canary && counts[1] == 0 &&
	           counts[2] == count_canary &&
	           FlagApi::KernelPollEventFlag(flag, second_pattern, 1, &observed_bits) == 0 &&
	           observed_bits == second_pattern,
	           "cancel without waiters did not reuse the flag, publish count zero or replace the full pattern") && ok;
	const bool deleted = Check(FlagApi::KernelDeleteEventFlag(flag) == 0 && FlagApi::EventFlagRegistryEmptyForTest(),
	                           "cancel fixture deletion retained a registry entry");
	return deleted && ok;
}

bool TestDeleteAndRegistry()
{
	if (!Check(FlagApi::EventFlagRegistryEmptyForTest(), "registry was not initially empty") ||
	    !Check(FlagApi::KernelDeleteEventFlag(nullptr) == Kernel::KERNEL_ERROR_ESRCH, "null delete did not return ESRCH"))
	{
		return false;
	}
	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-delete", 0x10, 0, nullptr) == 0 && flag != nullptr &&
	           !FlagApi::EventFlagRegistryEmptyForTest(), "create did not register a live flag")) return false;
	if (!Check(FlagApi::KernelDeleteEventFlag(flag) == 0 && FlagApi::EventFlagRegistryEmptyForTest(),
	           "delete did not release its registry entry") ||
	    !Check(FlagApi::ResetForNextSession() && FlagApi::EventFlagRegistryEmptyForTest(),
	           "session reset retained an explicitly deleted flag")) return false;
	flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-reset", 0x10, 0, nullptr) == 0 && flag != nullptr,
	           "reset fixture creation failed")) return false;
	return Check(FlagApi::ResetForNextSession() && FlagApi::EventFlagRegistryEmptyForTest(),
	             "session reset did not remove a live flag");
}

bool TestAttributeOneAdmission()
{
	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-attribute-one", 0x01, 0, nullptr) == 0 && flag != nullptr,
	           "event-flag attribute 0x01 was not admitted")) return false;

	int              first_wait_result = Kernel::KERNEL_ERROR_EBUSY;
	uint64_t         first_result_bits = 0;
	std::atomic_bool finished {false};
	std::thread waiter([flag, &first_wait_result, &first_result_bits, &finished] {
		first_wait_result = FlagApi::KernelWaitEventFlag(flag, 1, 1, &first_result_bits, nullptr);
		finished.store(true, std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool parked = false;
	while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		if (FlagApi::EventFlagHasWaitersForTest(flag))
		{
			parked = true;
			break;
		}
		std::this_thread::yield();
	}
	if (!parked)
	{
		FlagApi::KernelSetEventFlag(flag, 1);
		waiter.join();
		FlagApi::KernelDeleteEventFlag(flag);
		return Check(false, "attribute 0x01 waiter never reached the protected suspended state");
	}

	Kernel::KernelUseconds second_timeout = 0;
	uint64_t               second_result_bits = 0;
	const int second_wait_result = FlagApi::KernelWaitEventFlag(flag, 2, 1, &second_result_bits, &second_timeout);
	const int set_result = FlagApi::KernelSetEventFlag(flag, 1);
	waiter.join();

	bool ok = Check(second_wait_result == Kernel::KERNEL_ERROR_EPERM,
	                "attribute 0x01 did not retain the default single-waiter policy");
	ok = Check(set_result == 0 && first_wait_result == 0 && first_result_bits == 1,
	           "attribute 0x01 event flag did not complete its set/wait lifecycle") && ok;
	const bool deleted = Check(FlagApi::KernelDeleteEventFlag(flag) == 0 && FlagApi::EventFlagRegistryEmptyForTest(),
	                           "attribute 0x01 event flag retained a registry entry");
	return deleted && ok;
}

bool TestDeleteWithWaiter()
{
	constexpr uint64_t initial_bits = 0x80000000000000a0ull;
	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "event-flag-wait-delete", 0x10, initial_bits, nullptr) == 0 && flag != nullptr,
	           "wait fixture creation failed")) return false;
	int wait_result = 0;
	uint64_t result_bits = 0;
	std::atomic_bool finished {false};
	std::thread waiter([flag, &wait_result, &result_bits, &finished] {
		wait_result = FlagApi::KernelWaitEventFlag(flag, 1, 1, &result_bits, nullptr);
		finished.store(true, std::memory_order_release);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool parked = false;
	while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		if (FlagApi::EventFlagHasWaitersForTest(flag))
		{
			parked = true;
			break;
		}
		std::this_thread::yield();
	}
	if (!parked)
	{
		// Release a late waiter before deleting the fixture on this failure path.
		FlagApi::KernelSetEventFlag(flag, 1);
		waiter.join();
		FlagApi::KernelDeleteEventFlag(flag);
		return Check(false, "waiter never reached the protected suspended state");
	}
	const int deleted = FlagApi::KernelDeleteEventFlag(flag);
	waiter.join();
	return Check(deleted == 0 && wait_result == Kernel::KERNEL_ERROR_EACCES && result_bits == initial_bits,
	             "delete did not release its waiter with EACCES and the retained bits") &&
	       Check(FlagApi::EventFlagRegistryEmptyForTest() && FlagApi::ResetForNextSession(),
	             "waiter deletion retained a registry entry");
}

#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
namespace VM = Lumen::Core::VirtualMemory;
constexpr uint64_t kHostPage = 16384;

struct FexMapping final
{
	FexMapping(): address(VM::Alloc(0, kHostPage, VM::Mode::ReadWrite)) {}
	~FexMapping() { if (address != 0 && !VM::Free(address)) std::abort(); }
	const uint64_t address;
};

struct FexWaitContext final
{
	FlagApi::KernelEventFlag flag = nullptr;
	uint64_t code = 0;
	uint64_t result_address = 0;
	uint64_t timeout_address = 0;
	int wait_result = Kernel::KERNEL_ERROR_EBUSY;
	bool called = false;
};

void FexWaitTarget() {}

uint32_t FexWaitQuery(void* opaque, uint64_t address, uint64_t* base,
                      uint64_t* size, uint32_t* writable) noexcept
{
	const auto& context = *static_cast<FexWaitContext*>(opaque);
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

uint32_t FexWaitCallback(void* opaque, uint64_t target, uint64_t* gprs,
                         uint64_t* next_rip) noexcept
{
	auto& context = *static_cast<FexWaitContext*>(opaque);
	if (target != reinterpret_cast<uint64_t>(&FexWaitTarget))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	context.called = true;
	context.wait_result = FlagApi::KernelWaitEventFlag(
	    context.flag, 1, 1, reinterpret_cast<uint64_t*>(context.result_address),
	    reinterpret_cast<Kernel::KernelUseconds*>(context.timeout_address));
	gprs[0] = static_cast<uint64_t>(static_cast<int64_t>(context.wait_result));
	std::memcpy(next_rip, reinterpret_cast<const void*>(gprs[4]), sizeof(*next_rip));
	gprs[4] += sizeof(*next_rip);
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool TestFexWaitYieldsForMutation()
{
	FexMapping code, stack, outputs, mutation;
	if (!Check(code.address && stack.address && outputs.address && mutation.address,
	           "FEX wait fixture allocation failed")) return false;
	const uint64_t target = reinterpret_cast<uint64_t>(&FexWaitTarget);
	std::array<uint8_t, 13> program {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
	std::memcpy(program.data() + 2, &target, sizeof(target));
	std::memcpy(reinterpret_cast<void*>(code.address), program.data(), program.size());
	if (!Check(VM::Protect(code.address, kHostPage, VM::Mode::ExecuteRead) &&
	           LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1,
	           "FEX wait fixture code publication failed")) return false;
	FlagApi::KernelEventFlag flag = nullptr;
	if (!Check(FlagApi::KernelCreateEventFlag(&flag, "fex-wait-barrier", 0x10, 0, nullptr) == 0 && flag != nullptr,
	           "FEX wait fixture flag creation failed")) return false;
	FexWaitContext context {flag, code.address, outputs.address, outputs.address + 16};
	auto* result_bits = reinterpret_cast<uint64_t*>(context.result_address);
	auto* timeout = reinterpret_cast<Kernel::KernelUseconds*>(context.timeout_address);
	*result_bits = UINT64_C(0xfeedfacefeedface);
	*timeout = 8000000;
	std::atomic_bool finished {false};
	uint32_t run_result = UINT32_MAX;
	std::thread waiter([&] {
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = code.address;
		state.gprs[4] = stack.address + kHostPage - 8;
		const uint64_t root_return = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
		run_result = LumenAndroidFexCoreRunGuestV1(&state, FexWaitCallback, FexWaitQuery, &context);
		finished.store(true, std::memory_order_release);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	bool parked = false;
	while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		if (FlagApi::EventFlagHasWaitersForTest(flag)) { parked = true; break; }
		std::this_thread::yield();
	}
	// An Exclusive mapping change must be able to pass while the HLE waiter is
	// parked; before the Mac/FEX host-wait path it timed out behind its reader.
	const bool mutated = parked && VM::Protect(mutation.address, kHostPage, VM::Mode::Read);
	if (parked) *timeout = 0; // The wait must keep its host-owned input snapshot.
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	const bool still_waiting = parked && !finished.load(std::memory_order_acquire);
	const int set_result = FlagApi::KernelSetEventFlag(flag, 1);
	waiter.join();
	const bool ok = Check(parked && mutated && still_waiting && set_result == 0 &&
	                      context.called && context.wait_result == 0 &&
	                      run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	                      *result_bits == 1,
	                      "FEX wait failed to yield/readmit around Exclusive mutation or publish its result");
	return Check(FlagApi::KernelDeleteEventFlag(flag) == 0 && FlagApi::EventFlagRegistryEmptyForTest(),
	             "FEX wait fixture flag cleanup failed") && ok;
}
#endif

} // namespace

int main()
{
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Loader::SymbolDatabase symbols;
	ClearEventFlagFunction clear = nullptr;
	const Lumen::Loader::HostCallScalarMetadata* cancel = nullptr;
	if (!TestExport(&symbols, &clear, &cancel) || !TestClearThroughExport(clear) || !TestCancelThroughExport(*cancel) ||
	    !TestDeleteAndRegistry() || !TestAttributeOneAdmission() ||
	    !TestDeleteWithWaiter()
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
	    || !TestFexWaitYieldsForMutation()
#endif
	    ) return 1;
	std::puts("KernelEventFlagTest: PASS");
	return 0;
}
