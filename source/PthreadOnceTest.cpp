#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
#include <unistd.h>
#endif

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace Memory = Lumen::Core::VirtualMemory;
using OnceFunction = int (LUMEN_SYSV_ABI *)(const void*, Kernel::pthread_once_init_func_t);

static_assert(std::is_same_v<decltype(&Kernel::PthreadOnce), OnceFunction>);

OnceFunction                                 g_once = &Kernel::PthreadOnce;
const Lumen::Loader::HostCallScalarMetadata* g_scalar_metadata = nullptr;
std::atomic<unsigned>                        g_unexpected_calls {0};
std::atomic<unsigned>                        g_memory_calls {0};
std::atomic<unsigned>                        g_serial_calls {0};
std::atomic<unsigned>                        g_first_control_calls {0};
std::atomic<unsigned>                        g_second_control_calls {0};
std::atomic<unsigned>                        g_recursive_calls {0};
std::atomic<unsigned>                        g_nested_outer_calls {0};
std::atomic<unsigned>                        g_nested_inner_calls {0};
std::atomic<unsigned>                        g_contended_calls {0};
std::atomic<bool>                            g_contended_entered {false};
std::atomic<bool>                            g_release_contended {false};
const void*                                  g_recursive_control = nullptr;
const void*                                  g_nested_inner_control = nullptr;
int                                          g_recursive_result = 0;
int                                          g_nested_result = 0;
uint64_t                                     g_published_payload = 0;

constexpr uint64_t PUBLISHED_PAYLOAD = UINT64_C(0x6e2d08f4c1a7935b);

void LUMEN_SYSV_ABI UnexpectedInitializer()
{
	g_unexpected_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI MemoryInitializer()
{
	g_memory_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI SerialInitializer()
{
	g_serial_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI FirstControlInitializer()
{
	g_first_control_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI SecondControlInitializer()
{
	g_second_control_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI RecursiveInitializer()
{
	g_recursive_calls.fetch_add(1, std::memory_order_relaxed);
	g_recursive_result = g_once(g_recursive_control, &UnexpectedInitializer);
}

void LUMEN_SYSV_ABI NestedInnerInitializer()
{
	g_nested_inner_calls.fetch_add(1, std::memory_order_relaxed);
}

void LUMEN_SYSV_ABI NestedOuterInitializer()
{
	g_nested_outer_calls.fetch_add(1, std::memory_order_relaxed);
	g_nested_result = g_once(g_nested_inner_control, &NestedInnerInitializer);
}

void LUMEN_SYSV_ABI ContendedInitializer()
{
	g_contended_calls.fetch_add(1, std::memory_order_relaxed);
	g_contended_entered.store(true, std::memory_order_release);
	while (!g_release_contended.load(std::memory_order_acquire)) std::this_thread::yield();
	g_published_payload = PUBLISHED_PAYLOAD;
}

class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fputs("PthreadOnceTest: FAIL: operation timed out\n", stderr);
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}

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
	if (!condition) std::fprintf(stderr, "PthreadOnceTest: FAIL: %s\n", message);
	return condition;
}

bool Expect(int actual, int expected, const char* operation)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "PthreadOnceTest: FAIL: %s returned 0x%08x, expected 0x%08x\n", operation,
	             static_cast<unsigned>(actual), static_cast<unsigned>(expected));
	return false;
}

template <typename T>
bool WaitFor(const std::atomic<T>& value, T expected, const char* message)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do
	{
		if (value.load(std::memory_order_acquire) == expected) return true;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return Check(false, message);
}

class ControlMapping final
{
public:
	~ControlMapping()
	{
		if (m_address != 0) Memory::Free(m_address);
	}

	bool Initialize()
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
		const long detected_page_size = sysconf(_SC_PAGESIZE);
		if (!Check(detected_page_size > 0, "host page size is unavailable")) return false;
		m_page_size   = static_cast<size_t>(detected_page_size);
		m_size        = m_page_size * 3;
		m_valid_begin = m_page_size;
#else
		m_size = 4096;
#endif
		m_address = Memory::Alloc(0, m_size, Memory::Mode::ReadWrite);
		if (!Check(m_address != 0, "control mapping allocation failed")) return false;
		auto* bytes = Data();
		for (size_t index = 0; index < m_size; ++index)
			bytes[index] = static_cast<uint8_t>((index * 37U + 0x5bU) & 0xffU);
		m_original.assign(bytes, bytes + m_size);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
		if (!Check(Memory::Protect(m_address + m_valid_begin, m_page_size, Memory::Mode::Read),
		           "valid guest control page could not be made read-only")) return false;
		m_valid_read_only = true;
		if (!Check(Memory::Protect(m_address + 2 * m_page_size, m_page_size, Memory::Mode::NoAccess),
		           "invalid guest control page could not be guarded")) return false;
		m_invalid_guarded = true;
#endif
		return true;
	}

	const void* Control(size_t index) const
	{
		return Data() + m_valid_begin + 64 + index * 64;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
	const void* InvalidControl() const
	{
		return Data() + 2 * m_page_size + 64;
	}
#endif

	bool VerifyUnchanged()
	{
		bool restored = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
		if (m_valid_read_only)
		{
			restored = Check(Memory::Protect(m_address + m_valid_begin, m_page_size, Memory::Mode::ReadWrite),
			                 "read-only guest control page could not be restored") && restored;
			m_valid_read_only = false;
		}
		if (m_invalid_guarded)
		{
			restored = Check(Memory::Protect(m_address + 2 * m_page_size, m_page_size, Memory::Mode::ReadWrite),
			                 "guarded guest control page could not be restored") && restored;
			m_invalid_guarded = false;
		}
#endif
		return restored && Check(m_original.size() == m_size && std::memcmp(Data(), m_original.data(), m_size) == 0,
		                         "pthread once changed guest control bytes or surrounding canaries");
	}

	ControlMapping(const ControlMapping&)            = delete;
	ControlMapping& operator=(const ControlMapping&) = delete;
	ControlMapping()                                 = default;

private:
	uint8_t* Data() const { return reinterpret_cast<uint8_t*>(m_address); }

	uint64_t             m_address     = 0;
	size_t               m_size        = 0;
	size_t               m_valid_begin = 0;
	std::vector<uint8_t> m_original;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
	size_t m_page_size         = 0;
	bool   m_valid_read_only   = false;
	bool   m_invalid_guarded   = false;
#endif
};

Lumen::Loader::SymbolResolve ExactPthreadOnce()
{
	Lumen::Loader::SymbolResolve query {};
	query.name                 = "14bOACANTBo";
	query.library              = "libkernel";
	query.library_version      = 1;
	query.module               = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Lumen::Loader::SymbolType::Func;
	return query;
}

bool TestBinding(Lumen::Loader::SymbolDatabase* symbols)
{
	if (!Check(symbols != nullptr && Lumen::Libs::Init(U"libkernel_1", symbols), "libkernel registration failed")) return false;
	const auto  exact  = ExactPthreadOnce();
	const auto* symbol = symbols->Find(exact);
	if (!Check(symbol != nullptr && symbol->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadOnce) &&
	               symbol->dbg_name == U"LibKernel::PthreadOnce",
	           "pthread once NID or target binding differs")) return false;
	g_once            = reinterpret_cast<OnceFunction>(symbol->vaddr);
	g_scalar_metadata = symbols->HostCallScalarMetadataFor(symbol->vaddr);
	using ScalarType  = Lumen::Loader::HostCallScalarType;
	if (!Check(g_scalar_metadata != nullptr && g_scalar_metadata->arity == 2 &&
	               g_scalar_metadata->arguments[0] == ScalarType::Gp &&
	               g_scalar_metadata->arguments[1] == ScalarType::Gp &&
	               g_scalar_metadata->result == ScalarType::Gp && g_scalar_metadata->invoker != nullptr,
	           "pthread once scalar ABI metadata differs")) return false;

	for (unsigned mismatch = 0; mismatch < 7; ++mismatch)
	{
		auto wrong = exact;
		switch (mismatch)
		{
			case 0: wrong.name = "14bOACANTBp"; break;
			case 1: wrong.library = "unrelated_once_library"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module = "unrelated_once_module"; break;
			case 4: wrong.module_version_major = 2; break;
			case 5: wrong.module_version_minor = 0; break;
			case 6: wrong.type = Lumen::Loader::SymbolType::Object; break;
		}
		if (!Check(symbols->Find(wrong) == nullptr, "pthread once matched an unsupported symbol identity")) return false;
	}
	return Check(symbols->FindByStdName(U"14bOACANTBo") == nullptr &&
	                 symbols->FindByStdName(U"scePthreadOnce") == nullptr,
	             "pthread once gained an unqualified standard-name fallback");
}

bool TestErrors(const void* control)
{
	const unsigned before = g_unexpected_calls.load(std::memory_order_relaxed);
	std::array<uint64_t, 2> scalar_arguments {
	    0,
	    reinterpret_cast<uint64_t>(&UnexpectedInitializer),
	};
	const uint64_t expected_error = static_cast<uint32_t>(Kernel::KERNEL_ERROR_EINVAL);
	if (!Expect(g_once(nullptr, &UnexpectedInitializer), Kernel::KERNEL_ERROR_EINVAL, "null control") ||
	    !Expect(g_once(control, nullptr), Kernel::KERNEL_ERROR_EINVAL, "null initializer") ||
	    !Expect(g_once(nullptr, nullptr), Kernel::KERNEL_ERROR_EINVAL, "null control and initializer") ||
	    !Check(g_scalar_metadata != nullptr && g_scalar_metadata->invoker(scalar_arguments.data()) == expected_error,
	           "scalar invoker did not preserve EINVAL for a null control")) return false;
	scalar_arguments[0] = reinterpret_cast<uint64_t>(control);
	scalar_arguments[1] = 0;
	return Check(g_scalar_metadata->invoker(scalar_arguments.data()) == expected_error,
	             "scalar invoker did not preserve EINVAL for a null initializer") &&
	       Check(g_unexpected_calls.load(std::memory_order_relaxed) == before,
	             "a rejected pthread once call invoked its callback");
}

bool TestGuestMemory(const ControlMapping& controls)
{
	const void* valid = controls.Control(0);
	if (!Expect(g_once(valid, &MemoryInitializer), 0, "readable guest control") ||
	    !Expect(g_once(valid, &UnexpectedInitializer), 0, "completed readable guest control") ||
	    !Check(g_memory_calls.load(std::memory_order_relaxed) == 1,
	           "readable guest control did not run its initializer exactly once") ||
	    !Check(g_unexpected_calls.load(std::memory_order_relaxed) == 0,
	           "completed readable guest control selected another initializer")) return false;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)
	const unsigned before = g_unexpected_calls.load(std::memory_order_relaxed);
	return Expect(g_once(controls.InvalidControl(), &UnexpectedInitializer), Kernel::KERNEL_ERROR_EFAULT,
	              "no-access guest control") &&
	       Expect(g_once(reinterpret_cast<const void*>((std::numeric_limits<uintptr_t>::max)()), &UnexpectedInitializer),
	              Kernel::KERNEL_ERROR_EFAULT, "overflowing guest control") &&
	       Check(g_unexpected_calls.load(std::memory_order_relaxed) == before,
	             "invalid guest control invoked its initializer");
#else
	return true;
#endif
}

bool TestSerial(const void* control)
{
	return Expect(g_once(control, &SerialInitializer), 0, "serial first call") &&
	       Expect(g_once(control, &UnexpectedInitializer), 0, "serial second call") &&
	       Expect(g_once(control, &SerialInitializer), 0, "serial third call") &&
	       Check(g_serial_calls.load(std::memory_order_relaxed) == 1, "serial initializer did not run exactly once") &&
	       Check(g_unexpected_calls.load(std::memory_order_relaxed) == 0,
	             "completed serial control selected another initializer");
}

bool TestDistinctControls(const void* first, const void* second)
{
	return Check(first != second, "distinct guest controls share an address") &&
	       Expect(g_once(first, &FirstControlInitializer), 0, "first control initialization") &&
	       Expect(g_once(second, &SecondControlInitializer), 0, "second control initialization") &&
	       Expect(g_once(first, &SecondControlInitializer), 0, "first completed control") &&
	       Expect(g_once(second, &FirstControlInitializer), 0, "second completed control") &&
	       Check(g_first_control_calls.load(std::memory_order_relaxed) == 1 &&
	                 g_second_control_calls.load(std::memory_order_relaxed) == 1,
	             "distinct controls did not retain independent once states");
}

bool TestNestedAndRecursiveControls(const void* recursive, const void* outer, const void* inner)
{
	g_recursive_control    = recursive;
	g_nested_inner_control = inner;
	g_recursive_result     = 0;
	g_nested_result        = Kernel::KERNEL_ERROR_EINVAL;
	return Expect(g_once(recursive, &RecursiveInitializer), 0, "outer recursive initialization") &&
	       Expect(g_once(recursive, &UnexpectedInitializer), 0, "completed recursive control") &&
	       Expect(g_recursive_result, Kernel::KERNEL_ERROR_EDEADLK, "same-control recursive initialization") &&
	       Check(g_recursive_calls.load(std::memory_order_relaxed) == 1,
	             "recursive initializer ran more than once") &&
	       Expect(g_once(outer, &NestedOuterInitializer), 0, "outer distinct-control initialization") &&
	       Expect(g_nested_result, 0, "nested distinct-control initialization") &&
	       Expect(g_once(inner, &UnexpectedInitializer), 0, "completed nested inner control") &&
	       Check(g_nested_outer_calls.load(std::memory_order_relaxed) == 1 &&
	                 g_nested_inner_calls.load(std::memory_order_relaxed) == 1,
	             "nested distinct controls did not initialize independently") &&
	       Check(g_unexpected_calls.load(std::memory_order_relaxed) == 0,
	             "recursive or nested completed control selected another initializer");
}

bool TestContention(const void* control)
{
	constexpr size_t WAITER_COUNT = 11;
	g_contended_calls.store(0, std::memory_order_relaxed);
	g_contended_entered.store(false, std::memory_order_relaxed);
	g_release_contended.store(false, std::memory_order_relaxed);
	g_published_payload = 0;

	int      owner_result   = (std::numeric_limits<int>::min)();
	uint64_t owner_observed = 0;
	std::thread owner([&] {
		owner_result   = g_once(control, &ContendedInitializer);
		owner_observed = g_published_payload;
	});
	if (!WaitFor(g_contended_entered, true, "contended initializer did not enter"))
	{
		g_release_contended.store(true, std::memory_order_release);
		owner.join();
		return false;
	}

	std::atomic<unsigned> started {0};
	std::atomic<unsigned> returned {0};
	std::array<int, WAITER_COUNT> results {};
	std::array<uint64_t, WAITER_COUNT> observed {};
	std::array<std::thread, WAITER_COUNT> waiters;
	for (size_t index = 0; index < waiters.size(); ++index)
	{
		results[index] = (std::numeric_limits<int>::min)();
		waiters[index] = std::thread([&, index] {
			started.fetch_add(1, std::memory_order_release);
			results[index]  = g_once(control, &UnexpectedInitializer);
			observed[index] = g_published_payload;
			returned.fetch_add(1, std::memory_order_release);
		});
	}
	const bool all_started = WaitFor(started, static_cast<unsigned>(waiters.size()), "contending callers did not start");
	if (all_started) std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool blocked = Check(returned.load(std::memory_order_acquire) == 0,
	                           "a contending caller returned before initialization completed");
	g_release_contended.store(true, std::memory_order_release);
	owner.join();
	for (auto& waiter: waiters) waiter.join();

	bool results_ok = Expect(owner_result, 0, "contended owner result") &&
	                  Check(owner_observed == PUBLISHED_PAYLOAD, "owner did not observe initialized payload");
	for (size_t index = 0; index < waiters.size(); ++index)
	{
		results_ok = Expect(results[index], 0, "contended waiter result") && results_ok;
		results_ok = Check(observed[index] == PUBLISHED_PAYLOAD,
		                   "contended waiter returned before payload publication") && results_ok;
	}
	return all_started && blocked && results_ok &&
	       Check(g_contended_calls.load(std::memory_order_relaxed) == 1,
	             "contended initializer ran more than once") &&
	       Check(g_unexpected_calls.load(std::memory_order_relaxed) == 0,
	             "contending caller selected another initializer");
}

} // namespace

int main()
{
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);

	Lumen::Loader::SymbolDatabase symbols;
	if (!TestBinding(&symbols)) return 1;
	ControlMapping controls;
	if (!controls.Initialize()) return 1;
	bool passed = true;
	passed = TestErrors(controls.Control(1)) && passed;
	passed = TestGuestMemory(controls) && passed;
	passed = TestSerial(controls.Control(1)) && passed;
	passed = TestDistinctControls(controls.Control(2), controls.Control(3)) && passed;
	passed = TestNestedAndRecursiveControls(controls.Control(4), controls.Control(5), controls.Control(6)) && passed;
	passed = TestContention(controls.Control(7)) && passed;
	passed = controls.VerifyUnchanged() && passed;
	if (!passed) return 1;
	std::puts("PthreadOnceTest: PASS");
	return 0;
}
