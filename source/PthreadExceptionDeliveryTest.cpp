// Standalone macOS ARM64/FEX contract test for the PS4 exception delivery
// (libkernel_unity): handle resolution through the live pthread pool, queueing,
// the PS4 ucontext reconstruction and one real nested guest handler call
// driven with the interrupted HLE boundary state. A second case drives the
// generated-code boundary entry point, whose interrupted state is supplied
// explicitly (GP registers plus the reconstructed resume RIP) instead of the
// return-address slot of an HLE call. Every fixture is original synthetic
// x86-64 code; none of these bytes comes from a game or from FEX.
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
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
#include <mutex>
#include <thread>

#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
#error "PthreadExceptionDeliveryTest exercises the macOS ARM64/FEX host path"
#endif

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace VM     = Lumen::Core::VirtualMemory;

static_assert(Kernel::kExceptionContextSize >= 0x100, "context must cover mc_rsp");
static_assert(Kernel::kExceptionMcontextOffset == 0x40, "mcontext offset differs");
static_assert(Kernel::kExceptionMcRdiOffset == 0x48, "GP register block differs");
static_assert(Kernel::kExceptionMcRipOffset == 0xe0, "mc_rip offset differs");
static_assert(Kernel::kExceptionMcRspOffset == 0xf8, "mc_rsp offset differs");

class Watchdog final
{
public:
	explicit Watchdog(std::chrono::seconds timeout)
	    : m_thread([this, timeout]
	               {
		               std::unique_lock<std::mutex> lock(m_mutex);
		               if (!m_changed.wait_for(lock, timeout, [this] { return m_done; }))
		               {
			               std::fprintf(stderr, "PthreadExceptionDeliveryTest: FAIL: operation timed out\n");
			               std::fflush(stderr);
			               std::_Exit(2);
		               }
	               })
	{
	}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

	Watchdog(const Watchdog&)            = delete;
	Watchdog& operator=(const Watchdog&) = delete;

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	bool                    m_done = false;
	std::thread             m_thread;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "PthreadExceptionDeliveryTest: FAIL: %s\n", message);
	}
	return condition;
}

constexpr size_t kHostPage   = 16384;
constexpr size_t kStackOffset = 0x800;

// Managed R/W fixture mapping; the code mapping switches to RX before execution.
class Mapping final
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(VM::Alloc(0, size, VM::Mode::ReadWrite)) {}
	~Mapping()
	{
		if (m_address != 0 && !VM::Free(m_address)) std::abort();
	}
	Mapping(const Mapping&)            = delete;
	Mapping& operator=(const Mapping&) = delete;

	uint64_t Address() const { return m_address; }
	bool     Readable(size_t offset, void* destination, size_t size) const
	{
		if (m_address == 0 || offset > m_size || size > m_size - offset) return false;
		std::memcpy(destination, reinterpret_cast<const void*>(m_address + offset), size);
		return true;
	}
	bool Writable(size_t offset, const void* source, size_t size) const
	{
		if (m_address == 0 || offset > m_size || size > m_size - offset) return false;
		std::memcpy(reinterpret_cast<void*>(m_address + offset), source, size);
		return true;
	}

private:
	size_t   m_size;
	uint64_t m_address;
};

// [signum, ucontext] cells written by the synthetic guest handler.
struct HandlerResult
{
	uint64_t signum  = 0;
	uint64_t context = 0;
	uint64_t calls   = 0;
};

// Synthetic original x86-64 handler:
//   mov rax, <result page>; mov [rax], rdi; mov [rax+8], rsi; inc qword [rax+16]; ret
// The handler receives (signum, ucontext) in RDI/RSI and returns immediately.
class DeliveryFixture final
{
public:
	DeliveryFixture(): m_code(kHostPage), m_stack(kHostPage), m_result(kHostPage)
	{
		if (m_code.Address() == 0 || m_stack.Address() == 0 || m_result.Address() == 0)
		{
			std::_Exit(3);
		}
		std::array<uint8_t, 22> program {
		    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, imm64
		    0x48, 0x89, 0x38,                   // mov [rax], rdi
		    0x48, 0x89, 0x70, 0x08,             // mov [rax+8], rsi
		    0x48, 0xff, 0x40, 0x10,             // inc qword [rax+16]
		    0xc3,                               // ret
		};
		const uint64_t result_page = m_result.Address();
		std::memcpy(program.data() + 2, &result_page, sizeof(result_page));
		std::memcpy(reinterpret_cast<void*>(m_code.Address()), program.data(), program.size());
		if (!VM::Protect(m_code.Address(), kHostPage, VM::Mode::ExecuteRead) ||
		    !VM::FlushInstructionCache(m_code.Address(), program.size()))
		{
			std::_Exit(3);
		}
		// The interrupted HLE call pushed its return address at RSP; the
		// delivery reads that slot for mc_rip.
		const uint64_t return_address = m_code.Address();
		if (!m_stack.Writable(kStackOffset, &return_address, sizeof(return_address)))
		{
			std::_Exit(3);
		}
	}

	uint64_t Code() const { return m_code.Address(); }
	uint64_t ResultAddress() const { return m_result.Address(); }
	uint64_t InterruptedRsp() const { return m_stack.Address() + kStackOffset; }
	bool     ReadResult(HandlerResult* result) const { return m_result.Readable(0, result, sizeof(*result)); }
	bool     ResetResult() const { const HandlerResult result {}; return m_result.Writable(0, &result, sizeof(result)); }

private:
	Mapping m_code;
	Mapping m_stack;
	Mapping m_result;
};

std::array<uint64_t, 16> MakeGprs(uint64_t rsp)
{
	std::array<uint64_t, 16> gprs {};
	for (uint64_t index = 0; index < gprs.size(); ++index)
	{
		gprs[index] = UINT64_C(0x1000) + index * UINT64_C(0x11);
	}
	gprs[4] = rsp; // RSP
	return gprs;
}

bool TestContextLayout()
{
	const uint64_t rsp  = UINT64_C(0x0000007c12349000);
	const uint64_t rip  = UINT64_C(0x0000007183394288);
	const auto     gprs = MakeGprs(rsp);
	alignas(16) std::array<uint8_t, Kernel::kExceptionContextSize> context {};
	context.fill(0xaa);
	Kernel::PthreadBuildExceptionContext(gprs.data(), rip, context.data());

	const auto load = [&context](size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, context.data() + offset, sizeof(value));
		return value;
	};
	if (!Check(load(Kernel::kExceptionMcRdiOffset) == gprs[7] &&
	           load(Kernel::kExceptionMcRsiOffset) == gprs[6] &&
	           load(Kernel::kExceptionMcRdxOffset) == gprs[2] &&
	           load(Kernel::kExceptionMcRcxOffset) == gprs[1] &&
	           load(Kernel::kExceptionMcR8Offset) == gprs[8] &&
	           load(Kernel::kExceptionMcR9Offset) == gprs[9] &&
	           load(Kernel::kExceptionMcRaxOffset) == gprs[0] &&
	           load(Kernel::kExceptionMcRbxOffset) == gprs[3] &&
	           load(Kernel::kExceptionMcRbpOffset) == gprs[5] &&
	           load(Kernel::kExceptionMcR10Offset) == gprs[10] &&
	           load(Kernel::kExceptionMcR11Offset) == gprs[11] &&
	           load(Kernel::kExceptionMcR12Offset) == gprs[12] &&
	           load(Kernel::kExceptionMcR13Offset) == gprs[13] &&
	           load(Kernel::kExceptionMcR14Offset) == gprs[14] &&
	           load(Kernel::kExceptionMcR15Offset) == gprs[15],
	           "exception context lost an interrupted GP register"))
	{
		return false;
	}
	if (!Check(load(Kernel::kExceptionMcRipOffset) == rip &&
	           load(Kernel::kExceptionMcRspOffset) == rsp,
	           "exception context lost mc_rip or mc_rsp"))
	{
		return false;
	}
	// Everything the delivery does not reconstruct stays zero: the header, the
	// mc_onstack slot, the trap/segment/error fields, mc_cs, mc_rflags and every
	// byte after mc_rsp.
	const auto range_zero = [&context](size_t begin, size_t end)
	{
		for (size_t offset = begin; offset < end; ++offset)
		{
			if (context[offset] != 0) return false;
		}
		return true;
	};
	return Check(range_zero(0x00, Kernel::kExceptionMcRdiOffset) &&
	             range_zero(0xc0, 0xe0) &&
	             range_zero(0xe8, 0xf8) &&
	             range_zero(0x100, context.size()),
	             "exception context published unreconstructed fields");
}

int32_t CallRaise(Lumen::Loader::SymbolDatabase* symbols, uint64_t handle, int32_t signum)
{
	Lumen::Loader::SymbolResolve query {};
	query.name                 = "il03nluKfMk";
	query.library              = "libkernel_unity";
	query.library_version      = 1;
	query.module               = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	const auto* binding        = symbols->Find(query);
	if (!Check(binding != nullptr, "exact sceKernelRaiseException binding is absent"))
	{
		std::_Exit(3);
	}
	using RaiseException = int32_t(LUMEN_SYSV_ABI*)(uint64_t, int32_t);
	return reinterpret_cast<RaiseException>(binding->vaddr)(handle, signum);
}

bool TestDelivery(Lumen::Loader::SymbolDatabase* symbols, const DeliveryFixture& fixture)
{
	const uint64_t self = reinterpret_cast<uint64_t>(Kernel::PthreadSelf());
	if (!Check(self != 0, "main thread handle is unavailable")) return false;

	// The console raise entry point rejects every signal but SIGUSR1, and an
	// unknown handle fails closed without queueing anything.
	if (!Check(CallRaise(symbols, self, 29) == Kernel::KERNEL_ERROR_EINVAL,
	           "raise accepted a signal other than SIGUSR1") ||
	    !Check(CallRaise(symbols, 0x1234, 30) == Kernel::KERNEL_ERROR_ESRCH,
	           "raise accepted an unknown pthread handle"))
	{
		return false;
	}

	// A queued request survives a delivery attempt without an interrupted
	// state: it stays pending for the next boundary of the thread.
	if (!Check(CallRaise(symbols, self, 30) == 0, "live thread handle did not accept a raise") ||
	    !Check(!Kernel::PthreadDeliverPendingException(nullptr),
	           "delivery ran without an interrupted boundary state"))
	{
		return false;
	}

	auto gprs = MakeGprs(fixture.InterruptedRsp());
	gprs[7]   = UINT64_C(0x0000007c11223344); // RDI of the interrupted call
	if (!Check(Kernel::PthreadDeliverPendingException(gprs.data()),
	           "queued exception did not deliver with the boundary state"))
	{
		return false;
	}

	HandlerResult result {};
	if (!Check(fixture.ReadResult(&result), "handler result page is unreadable")) return false;
	if (!Check(result.signum == 30, "handler did not receive SIGUSR1 as its first argument")) return false;
	const uint64_t context_address = result.context;
	if (!Check(context_address != 0, "handler did not receive a ucontext pointer")) return false;
	const auto* context = reinterpret_cast<const uint8_t*>(context_address);
	const auto  load    = [context](size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, context + offset, sizeof(value));
		return value;
	};
	if (!Check(load(Kernel::kExceptionMcRdiOffset) == gprs[7] &&
	           load(Kernel::kExceptionMcRspOffset) == gprs[4] &&
	           load(Kernel::kExceptionMcRipOffset) == fixture.Code(),
	           "guest handler observed a different interrupted state"))
	{
		return false;
	}

	// The request was consumed exactly once: the handler ran nested on this
	// thread and the next boundary has nothing left to deliver.
	return Check(!Kernel::PthreadDeliverPendingException(gprs.data()),
	             "exception request was delivered twice");
}

bool TestBoundaryDelivery(Lumen::Loader::SymbolDatabase* symbols, const DeliveryFixture& fixture)
{
	const uint64_t self = reinterpret_cast<uint64_t>(Kernel::PthreadSelf());
	if (!Check(self != 0, "main thread handle is unavailable")) return false;

	// The generated-code route publishes the interrupted RIP of the boundary
	// directly; the return-address slot of the fixture stack holds a different
	// value, so a fallback to the HLE reconstruction would be observable.
	const uint64_t boundary_rip = fixture.Code() + 7;
	auto gprs = MakeGprs(fixture.InterruptedRsp());
	gprs[7]   = UINT64_C(0x0000007c55667788);
	if (!Check(!Kernel::PthreadDeliverPendingExceptionAtBoundary(gprs.data(), boundary_rip),
	           "boundary delivery ran without a queued request"))
	{
		return false;
	}

	if (!Check(CallRaise(symbols, self, 30) == 0, "live thread handle did not accept a raise") ||
	    !Check(Kernel::PthreadDeliverPendingExceptionAtBoundary(gprs.data(), boundary_rip),
	           "queued exception did not deliver at the generated-code boundary"))
	{
		return false;
	}

	HandlerResult result {};
	if (!Check(fixture.ReadResult(&result), "handler result page is unreadable")) return false;
	if (!Check(result.signum == 30, "boundary handler did not receive SIGUSR1")) return false;
	const auto* context = reinterpret_cast<const uint8_t*>(result.context);
	if (!Check(context != nullptr, "boundary handler did not receive a ucontext pointer")) return false;
	const auto load = [context](size_t offset)
	{
		uint64_t value = 0;
		std::memcpy(&value, context + offset, sizeof(value));
		return value;
	};
	if (!Check(load(Kernel::kExceptionMcRipOffset) == boundary_rip &&
	               load(Kernel::kExceptionMcRspOffset) == gprs[4] &&
	               load(Kernel::kExceptionMcRdiOffset) == gprs[7],
	           "boundary handler observed a different interrupted state"))
	{
		return false;
	}

	// The request was consumed exactly once through this route as well.
	return Check(!Kernel::PthreadDeliverPendingExceptionAtBoundary(gprs.data(), boundary_rip),
	             "boundary exception request was delivered twice");
}

bool TestShortCallDelivery(Lumen::Loader::SymbolDatabase* symbols, const DeliveryFixture& fixture)
{
	const auto target = reinterpret_cast<uint64_t>(&Kernel::PthreadSelf);
	const auto self = reinterpret_cast<uint64_t>(Kernel::PthreadSelf());
	if (!Check(symbols->ResolveHostCall(target).effects == Lumen::GuestCpu::HostCallEffect::None,
	           "pthread self lacks audited short-call effects")) return false;
	Mapping code(kHostPage);
	std::array<uint8_t, 21> program {
	    0x48, 0x83, 0xec, 0x08,             // sub rsp,8
	    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax,PthreadSelf
	    0xff, 0xd0,                         // call rax (return at +16)
	    0x48, 0x83, 0xc4, 0x08, 0xc3};     // add rsp,8; ret
	std::memcpy(program.data() + 6, &target, sizeof(target));
	if (!Check(code.Address() != 0 && code.Writable(0, program.data(), program.size()) &&
	           VM::Protect(code.Address(), kHostPage, VM::Mode::ExecuteRead) &&
	           VM::FlushInstructionCache(code.Address(), program.size()) && fixture.ResetResult(),
	           "short-call exception fixture publication failed")) return false;
	// Queue before Run: no active translated thread can receive a generated-code
	// interrupt. Delivery must occur in the actual PthreadSelf HLE bridge, after
	// its short admission and before the native service returns its handle.
	if (!Check(CallRaise(symbols, self, 30) == 0 &&
	           Lumen::Loader::RunGuestX64(code.Address()) == self,
	           "queued handler or pthread self result failed under short admission")) return false;
	HandlerResult result {};
	uint64_t interrupted_rip {};
	if (!Check(fixture.ReadResult(&result) && result.signum == 30 && result.context != 0 &&
	           Lumen::Loader::ReadGuestMemory(result.context + Kernel::kExceptionMcRipOffset,
	               &interrupted_rip, sizeof(interrupted_rip)) && interrupted_rip == code.Address() + 16 &&
	           !Kernel::PthreadDeliverPendingException(nullptr),
	           "short-call callback was deferred, duplicated, or delivered at another boundary")) return false;
	std::puts("PthreadExceptionDeliveryTest: short-call delivery PASS");
	return true;
}

struct SleepRendezvous
{
	std::atomic_uint32_t slices {0};
	std::atomic_bool raised {false}, observed {false};
};
static thread_local SleepRendezvous* sleep_rendezvous;

void SleepSliceRendezvous(uint64_t nanos)
{
	if (nanos == 0 || nanos > 10000000) std::abort();
	const auto slice = sleep_rendezvous->slices.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (slice == 1)
		while (!sleep_rendezvous->raised.load(std::memory_order_acquire)) std::this_thread::yield();
	if (slice == 2)
		while (!sleep_rendezvous->observed.load(std::memory_order_acquire)) std::this_thread::yield();
}

bool TestSleepDelivery(Lumen::Loader::SymbolDatabase* symbols, const DeliveryFixture& fixture)
{
	const uint64_t self = reinterpret_cast<uint64_t>(Kernel::PthreadSelf());
	const auto target = reinterpret_cast<uint64_t>(&Kernel::KernelUsleep);
	Mapping code(kHostPage);
	// sub rsp,8; mov edi,25000; movabs rax,KernelUsleep; call rax; add rsp,8; ret
	std::array<uint8_t, 26> program {
	    0x48, 0x83, 0xec, 0x08, 0xbf, 0xa8, 0x61, 0, 0,
	    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
	    0x48, 0x83, 0xc4, 0x08, 0xc3};
	std::memcpy(program.data() + 11, &target, sizeof(target));
	if (!Check(self != 0 && code.Address() != 0 && code.Writable(0, program.data(), program.size()) &&
	           VM::Protect(code.Address(), kHostPage, VM::Mode::ExecuteRead) &&
	           VM::FlushInstructionCache(code.Address(), program.size()) && fixture.ResetResult(),
	           "sleep exception fixture publication failed")) return false;
	SleepRendezvous rendezvous;
	sleep_rendezvous = &rendezvous;
	Kernel::PthreadSetSleepSliceTestHook(SleepSliceRendezvous);
	bool raised = false, delivered_while_sleeping = false;
	std::thread raiser([&] {
		while (rendezvous.slices.load(std::memory_order_acquire) < 1) std::this_thread::yield();
		raised = CallRaise(symbols, self, 30) == 0;
		rendezvous.raised.store(true, std::memory_order_release);
		while (rendezvous.slices.load(std::memory_order_acquire) < 2) std::this_thread::yield();
		// The sleeper remains quiescent. A separate host reader acquires fresh
		// leases instead of accessing guest bytes in the native sleep callback.
		HandlerResult result {};
		uint64_t rip = 0;
		delivered_while_sleeping = Lumen::Loader::ReadGuestMemory(fixture.ResultAddress(), &result, sizeof(result)) &&
		    result.signum == 30 && result.context != 0 && result.calls == 1 &&
		    Lumen::Loader::ReadGuestMemory(result.context + Kernel::kExceptionMcRipOffset, &rip, sizeof(rip)) &&
		    rip == code.Address() + 21;
		rendezvous.observed.store(true, std::memory_order_release);
	});
	const auto returned = Lumen::Loader::RunGuestX64(code.Address());
	raiser.join();
	Kernel::PthreadSetSleepSliceTestHook(nullptr);
	sleep_rendezvous = nullptr;
	HandlerResult final_result {};
	if (!Check(returned == 0 && raised && delivered_while_sleeping && rendezvous.slices == 3 &&
	           fixture.ReadResult(&final_result) && final_result.calls == 1 &&
	           !Kernel::PthreadDeliverPendingException(nullptr),
	           "sleep callback missed slice delivery, changed result/duration, or left a duplicate request")) return false;
	std::puts("PthreadExceptionDeliveryTest: native sleep slice delivery PASS");
	return true;
}

} // namespace

int main()
{
	Watchdog watchdog(std::chrono::seconds(20));
	if (setenv("LUMEN_MACOS_FEXCORE_RUN", "1", 1) != 0) return 1;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	VM::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	Kernel::PthreadInitSelfForMainThread();

	LumenAndroidFexCoreRuntimeStatusV1 status {};
	if (!Check(LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	           status.backendRouteReady != 0, "real FEX runtime unavailable")) return 1;

	// This fixture only supplies a TLS owner for nested guest calls; the
	// pthread pool, handle resolution, queueing and delivery use their
	// production paths.
	auto* linker = Lumen::Core::Singleton<Lumen::Loader::RuntimeLinker>::Instance();
	if (!Check(linker != nullptr && linker->SetupSyntheticTlsForPthreadExitTest(),
	           "synthetic TLS owner setup failed")) return 1;
	auto* symbols = linker->Symbols();
	if (!Check(symbols != nullptr && Lumen::Libs::Init(U"libkernel_1", symbols),
	           "libkernel registration failed")) { linker->Clear(); return 1; }
	symbols->SealHostCallRegistry();

	Lumen::Loader::SymbolResolve query {};
	query.name                 = "WkwEd3N7w0Y";
	query.library              = "libkernel_unity";
	query.library_version      = 1;
	query.module               = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	const auto* install_binding = symbols->Find(query);
	using InstallExceptionHandler = int32_t(LUMEN_SYSV_ABI*)(int32_t, uint64_t);
	if (!Check(install_binding != nullptr, "exact sceKernelInstallExceptionHandler binding is absent"))
	{
		linker->Clear();
		return 1;
	}
	const auto install = reinterpret_cast<InstallExceptionHandler>(install_binding->vaddr);

	DeliveryFixture fixture;
	const bool passed = TestContextLayout() &&
	                    Check(install(30, fixture.Code()) == 0, "SIGUSR1 handler installation failed") &&
	                    TestDelivery(symbols, fixture) &&
	                    TestBoundaryDelivery(symbols, fixture) &&
	                    TestShortCallDelivery(symbols, fixture) &&
	                    TestSleepDelivery(symbols, fixture);
	linker->Clear();
	if (!passed) return 1;
	std::puts("PthreadExceptionDeliveryTest: PASS");
	return 0;
}
