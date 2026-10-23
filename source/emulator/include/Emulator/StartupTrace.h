#ifndef LUMEN_EMULATOR_STARTUP_TRACE_H_
#define LUMEN_EMULATOR_STARTUP_TRACE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Lumen::Emulator::StartupTrace {

inline bool Enabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_STARTUP_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

inline long long MonotonicNanoseconds()
{
	return static_cast<long long>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr unsigned MaximumRecords = 128;

struct Record
{
	std::atomic_bool ready {false};
	const char*      component    = nullptr;
	const char*      phase        = nullptr;
	long long        monotonic_ns = 0;
};

inline std::array<Record, MaximumRecords>& Records()
{
	static std::array<Record, MaximumRecords> records {};
	return records;
}

inline std::atomic_uint& NextRecord()
{
	static std::atomic_uint next {0};
	return next;
}

inline std::atomic_flag& RecordLock()
{
	static std::atomic_flag lock = ATOMIC_FLAG_INIT;
	return lock;
}

inline std::atomic_flag& FlushLock()
{
	static std::atomic_flag lock = ATOMIC_FLAG_INIT;
	return lock;
}

inline std::atomic_bool& TruncationReported()
{
	static std::atomic_bool reported {false};
	return reported;
}

inline std::atomic_bool& Closed()
{
	static std::atomic_bool closed {false};
	return closed;
}

inline void Acquire(std::atomic_flag& lock)
{
	while (lock.test_and_set(std::memory_order_acquire))
	{
	}
}

inline void Release(std::atomic_flag& lock)
{
	lock.clear(std::memory_order_release);
}

#if defined(LUMEN_STARTUP_TRACE_TEST_HOOK)
using TestHook = void (*)();

inline std::atomic<TestHook>& EmitAdmittedTestHook()
{
	static std::atomic<TestHook> hook {nullptr};
	return hook;
}

inline std::atomic<TestHook>& FlushWaitingTestHook()
{
	static std::atomic<TestHook> hook {nullptr};
	return hook;
}
#endif

// RecordLock must be held and the epoch must still be open.
inline void Append(const char* component, const char* phase, long long monotonic_ns)
{
	const unsigned index = NextRecord().load(std::memory_order_relaxed);
	if (index >= MaximumRecords)
	{
		// Saturate one past the bounded buffer so Flush() can report truncation
		// without allowing an unbounded counter to wrap back into valid slots.
		NextRecord().store(MaximumRecords + 1, std::memory_order_release);
		return;
	}
	auto& record        = Records()[index];
	record.component    = component;
	record.phase        = phase;
	record.monotonic_ns = monotonic_ns;
	record.ready.store(true, std::memory_order_release);
	// Publish the count only after the complete record. Flush therefore never
	// observes a reserved-but-unready gap between concurrent producers.
	NextRecord().store(index + 1, std::memory_order_release);
}

inline void Emit(const char* component, const char* phase)
{
	if (!Enabled() || component == nullptr || phase == nullptr)
	{
		return;
	}
	Acquire(RecordLock());
	if (Closed().load(std::memory_order_acquire))
	{
		Release(RecordLock());
		return;
	}
#if defined(LUMEN_STARTUP_TRACE_TEST_HOOK)
	if (const auto hook = EmitAdmittedTestHook().load(std::memory_order_acquire); hook != nullptr)
	{
		hook();
	}
#endif
	// Timestamp and admission share the same serialized linearization point. A
	// Flush either waits for this complete record or closes before it begins.
	Append(component, phase, MonotonicNanoseconds());
	Release(RecordLock());
}

inline void Flush()
{
	if (!Enabled())
	{
		return;
	}
	Acquire(FlushLock());
#if defined(LUMEN_STARTUP_TRACE_TEST_HOOK)
	if (const auto hook = FlushWaitingTestHook().load(std::memory_order_acquire); hook != nullptr)
	{
		hook();
	}
#endif
	Acquire(RecordLock());
	if (Closed().load(std::memory_order_acquire))
	{
		Release(RecordLock());
		Release(FlushLock());
		return;
	}
	// Closing under the same lock used by producers makes the trace a fixed
	// startup epoch. No record can be appended after this snapshot.
	Closed().store(true, std::memory_order_release);
	const unsigned limit = (std::min)(NextRecord().load(std::memory_order_acquire), MaximumRecords);
	Release(RecordLock());
	for (unsigned index = 0; index < limit; ++index)
	{
		const auto& record = Records()[index];
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "LumenStartupTrace",
		                    "LUMEN_STARTUP_TRACE v=1 clock=steady component=%s phase=%s monotonic_ns=%lld",
		                    record.component, record.phase, record.monotonic_ns);
#else
		std::fprintf(stderr, "LUMEN_STARTUP_TRACE v=1 clock=steady component=%s phase=%s monotonic_ns=%lld\n",
		             record.component, record.phase, record.monotonic_ns);
#endif
	}
	if (NextRecord().load(std::memory_order_acquire) > MaximumRecords && limit == MaximumRecords &&
	    !TruncationReported().exchange(true, std::memory_order_acq_rel))
	{
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_WARN, "LumenStartupTrace",
		                    "LUMEN_STARTUP_TRACE v=1 clock=steady component=trace phase=truncated monotonic_ns=%lld",
		                    MonotonicNanoseconds());
#else
		std::fprintf(stderr, "LUMEN_STARTUP_TRACE v=1 clock=steady component=trace phase=truncated monotonic_ns=%lld\n",
		             MonotonicNanoseconds());
#endif
	}
#if !defined(__ANDROID__)
	std::fflush(stderr);
#endif
	Release(FlushLock());
}

// The iOS runtime is process-lifetime and can launch multiple isolated guest
// sessions. Call this only at a quiescent session boundary, after the previous
// session has been flushed and all of its producers have stopped.
inline void Reset()
{
	if (!Enabled())
	{
		return;
	}
	Acquire(FlushLock());
	Acquire(RecordLock());
	for (auto& record: Records())
	{
		record.ready.store(false, std::memory_order_relaxed);
		record.component    = nullptr;
		record.phase        = nullptr;
		record.monotonic_ns = 0;
	}
	NextRecord().store(0, std::memory_order_release);
	TruncationReported().store(false, std::memory_order_release);
	// Re-open only after every slot and counter belongs to the new epoch.
	Closed().store(false, std::memory_order_release);
	Release(RecordLock());
	Release(FlushLock());
}

} // namespace Lumen::Emulator::StartupTrace

#endif // LUMEN_EMULATOR_STARTUP_TRACE_H_
