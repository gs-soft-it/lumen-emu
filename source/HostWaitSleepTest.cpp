// Production slice helper and real FEX barrier; native sleeps and exception
// callbacks are deterministic host fixtures, not executed guest instructions.
#define LUMEN_ANDROID_FEXCORE_BARRIER_TESTING 1
#include "../android/app/src/main/cpp/AndroidFexCoreMutationBarrier.h"
#include "emulator/include/Emulator/Kernel/HostWaitSleep.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using Barrier = Lumen::Android::FexCoreLab::GuestMemoryMutationBarrier;
using Lumen::Libs::LibKernel::Detail::SleepWithHostWaitSlices;
constexpr uint64_t Slice = 10000000;

void Require(bool value, const char* message)
{
	if (!value) { std::fprintf(stderr, "HostWaitSleepTest: FAIL: %s\n", message); std::abort(); }
}

struct Context
{
	Barrier barrier;
	std::atomic_bool pending {false}, restoring {false};
	unsigned begins = 0, ends = 0, active = 0, deliveries = 0;
	uint64_t slept = 0;
	bool delivering = false, nested_delivery = false;
	bool expect_reader = true;
	std::vector<uint64_t> slices, delivered_at;
};
thread_local Context* current;

class HostWait
{
public:
	HostWait(): context(*current), token(context.barrier.BeginQuiescentHostWait())
	{ ++context.begins; if (token != 0) ++context.active; }
	~HostWait()
	{
		++context.ends;
		context.restoring.store(true, std::memory_order_release);
		if (token != 0) Require(context.barrier.EndQuiescentHostWait(token), "readmission failed");
		if (context.delivering || !context.pending.exchange(false)) return;
		Require(!context.expect_reader || context.barrier.CurrentThreadOwnsHostMemoryReadLease(),
		        "exception callback ran before reader restoration");
		++context.deliveries;
		context.delivered_at.push_back(context.slept);
		context.delivering = true;
		if (context.nested_delivery)
		{
			Require(context.barrier.EnterExecution() && context.barrier.SuspendExecutionForHostCall(),
			        "nested handler entry failed");
			const auto active_before = context.active;
			const auto begins_before = context.begins;
			SleepWithHostWaitSlices<HostWait>(2 * Slice, true, [](uint64_t) {}, [] { return false; });
			Require(context.active == active_before && context.begins == begins_before + 2,
			        "nested wait stole outer reader or lost slice boundaries");
			Require(context.barrier.ResumeExecutionAfterHostCall(), "nested handler return failed");
			context.barrier.LeaveExecution();
		}
		context.delivering = false;
	}
	bool IsActive() const { return token != 0; }
private:
	Context& context;
	uint64_t token;
};

void Enter(Context& c)
{
	current = &c;
	Require(c.barrier.EnterExecution() && c.barrier.SuspendExecutionForHostCall(), "HLE entry failed");
}

void Leave(Context& c)
{
	Require(c.barrier.ResumeExecutionAfterHostCall(), "HLE return failed");
	c.barrier.LeaveExecution();
	const auto s = c.barrier.SnapshotForTesting();
	Require(s.readers == 0 && s.hostCallReaders == 0 && s.parked == 0 && s.waitingEntrants == 0 &&
	        s.requestEpoch == 0 && !s.writer && !s.fullIntent, "unbalanced barrier after sleep");
	current = nullptr;
}

void Sleep(Context& c, uint64_t ns)
{
	Require(ns > 0 && ns <= Slice, "native slice exceeds exception-check bound");
	c.slices.push_back(ns);
	c.slept += ns;
}

void DurationAndMechanism()
{
	for (uint64_t duration: {uint64_t {0}, uint64_t {1}, Slice, Slice + 1, 5 * Slice + 7})
	{
		std::vector<uint64_t> before;
		for (bool retain: {false, true})
		{
			Context c;
			Enter(c);
			unsigned mutations = 0;
			SleepWithHostWaitSlices<HostWait>(duration, retain, [&](uint64_t ns) {
				Sleep(c, ns);
				Require(!c.barrier.CurrentThreadOwnsHostMemoryReadLease(), "native sleep retained reader");
				std::thread writer([&] {
					const auto token = c.barrier.BeginMutation();
					Require(token != 0 && c.barrier.EndMutation(token), "Exclusive cannot progress during sleep");
					++mutations;
				});
				writer.join();
			}, [&] { return c.pending.load(std::memory_order_acquire); });
			Require(c.slept == duration && mutations == c.slices.size(), "duration or writer progress changed");
			const auto expected = retain ? unsigned(duration != 0) : unsigned(c.slices.size());
			Require(c.begins == expected && c.ends == expected && c.active == expected,
			        "unexpected sleep admission count");
			if (!retain) before = c.slices;
			else Require(c.slices == before, "candidate changed native sleep requests");
			if (duration == 5 * Slice + 7)
				std::printf("HostWaitSleepTest: %s slices=%zu admissions=%u requested_ns=%llu\n",
				    retain ? "AFTER" : "BEFORE", c.slices.size(), c.begins,
				    static_cast<unsigned long long>(c.slept));
			Leave(c);
		}
	}
}

void ExceptionBoundaries()
{
	for (bool retain: {false, true})
	{
		for (unsigned arrival: {0u, 1u, 2u, 3u})
		{
			Context c;
			Enter(c);
			c.pending = arrival == 0;
			c.nested_delivery = true;
			unsigned slice = 0;
			SleepWithHostWaitSlices<HostWait>(3 * Slice, retain, [&](uint64_t ns) {
				Sleep(c, ns);
				if (++slice == arrival) c.pending.store(true, std::memory_order_release);
			}, [&] { return c.pending.load(std::memory_order_acquire); });
			Require(c.deliveries == 1 && c.delivered_at[0] == (arrival == 0 ? 1 : arrival) * Slice,
			        "exception slice changed or exception delivered twice");
			Require(c.slept == 3 * Slice, "exception handler changed remaining requested duration");
			Leave(c);
		}
	}
	// Deterministically place a raise just after a clean pending check.
	Context c;
	Enter(c);
	unsigned checks = 0;
	SleepWithHostWaitSlices<HostWait>(3 * Slice, true, [&](uint64_t ns) { Sleep(c, ns); }, [&] {
		const bool result = c.pending.load(std::memory_order_acquire);
		if (++checks == 1) c.pending.store(true, std::memory_order_release);
		return result;
	});
	Require(c.deliveries == 1 && c.delivered_at[0] == 2 * Slice, "raise racing probe missed next slice");
	Leave(c);
}

void IneligibleWaits()
{
	for (unsigned mode = 0; mode < 3; ++mode)
	{
		Context c;
		current = &c;
		if (mode != 0) Enter(c);
		else c.expect_reader = false;
		if (mode == 1) Require(c.barrier.RetainGuestDataLease(), "fixture borrow failed");
		if (mode == 2)
			Require(c.barrier.EnterExecution() && c.barrier.SuspendExecutionForHostCall(), "fixture nested entry failed");
		SleepWithHostWaitSlices<HostWait>(3 * Slice, true, [&](uint64_t ns) { Sleep(c, ns); }, [] {
			Require(false, "ineligible wait used retained pending probe"); return false;
		});
		Require(c.active == 0 && c.begins == 3 && c.ends == 3, "ineligible wait changed slice boundaries");
		if (mode == 1) Require(c.barrier.ReleaseGuestDataLease(), "fixture borrow release failed");
		if (mode == 2) { Require(c.barrier.ResumeExecutionAfterHostCall(), "fixture nested return failed"); c.barrier.LeaveExecution(); }
		if (mode != 0) Leave(c);
		current = nullptr;
	}
}

void FinishWaitsForPublication()
{
	Context c;
	Enter(c);
	std::atomic_bool writer_active {false}, published {false};
	std::thread writer;
	SleepWithHostWaitSlices<HostWait>(Slice, true, [&](uint64_t ns) {
		Sleep(c, ns);
		writer = std::thread([&] {
			const auto token = c.barrier.BeginMutation();
			Require(token != 0, "fixture writer could not acquire Exclusive");
			writer_active.store(true, std::memory_order_release);
			while (!c.restoring.load(std::memory_order_acquire)) std::this_thread::yield();
			c.pending.store(true, std::memory_order_release);
			published.store(true, std::memory_order_release);
			Require(c.barrier.EndMutation(token), "fixture writer could not publish");
		});
		while (!writer_active.load(std::memory_order_acquire)) std::this_thread::yield();
	}, [] { return false; });
	writer.join();
	Require(published.load(std::memory_order_acquire) && c.deliveries == 1,
	        "final return preceded publication or lost exception queued during restore");
	Leave(c);
}
} // namespace

int main()
{
	DurationAndMechanism();
	ExceptionBoundaries();
	IneligibleWaits();
	FinishWaitsForPublication();
	std::puts("HostWaitSleepTest: PASS");
}
