// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/host_gpu/renderer/pipeline/driverCacheCheckpoint.h"

#include <cstdio>
#include <future>
#include <stdexcept>

using namespace std::chrono_literals;
using Libs::Graphics::DriverCacheCheckpoint::SavedPayload;
using Libs::Graphics::DriverCacheCheckpoint::Worker;

namespace {

void Require(bool value, const char* label) {
	if (!value) throw std::runtime_error(label);
}

struct Gate {
	std::mutex mutex;
	std::condition_variable condition;
	unsigned entered = 0;
	unsigned allowed = 0;
	unsigned completed = 0;

	void WaitFor(unsigned& field, unsigned value) {
		std::unique_lock lock(mutex);
		Require(condition.wait_for(lock, 2s, [&] { return field >= value; }), "worker deadline");
	}
	void Allow(unsigned value) {
		std::lock_guard lock(mutex);
		allowed = value;
		condition.notify_all();
	}
	bool Callback() {
		std::unique_lock lock(mutex);
		const auto current = ++entered;
		condition.notify_all();
		Require(condition.wait_for(lock, 2s, [&] { return allowed >= current; }), "callback gate");
		++completed;
		condition.notify_all();
		return true;
	}
};

void PayloadContracts() {
	SavedPayload saved({1, 2, 3});
	unsigned writes = 0;
	bool success = true;
	const auto write = [&](std::span<const uint8_t>) { ++writes; return success; };
	Require(saved.Save({1, 2, 3}, write) && writes == 0, "loaded payload is not rewritten");
	Require(saved.Save({1, 9, 3}, write) && writes == 1, "same-size byte change is written");
	Require(saved.Save({1, 9, 3}, write) && writes == 1, "saved payload is not rewritten");
	success = false;
	Require(!saved.Save({1, 9, 4}, write) && writes == 2, "write failure is returned");
	success = true;
	Require(saved.Save({1, 9, 4}, write) && writes == 3, "failed payload is retried");
	bool threw = false;
	try {
		(void)saved.Save({2, 9, 4}, [](auto) -> bool { throw std::runtime_error("write"); });
	} catch (const std::runtime_error&) { threw = true; }
	Require(threw, "write exception must propagate to worker");
	Require(saved.Save({2, 9, 4}, write) && writes == 4, "throwing write keeps last durable payload");
}

void CoalescingAndShutdownContracts() {
	Gate gate;
	Worker worker([&] { return gate.Callback(); }, 5min);
	for (unsigned i = 0; i < 64; ++i) worker.MarkDirty();
	worker.Request();
	gate.WaitFor(gate.entered, 1);
	for (unsigned i = 0; i < 64; ++i) worker.MarkDirty();
	// Stop must wait for an in-flight write and then checkpoint the newer
	// generation; it cannot destroy callback resources after just the first save.
	auto stopped = std::async(std::launch::async, [&] { return worker.Stop(); });
	Require(stopped.wait_for(10ms) == std::future_status::timeout, "stop joins in-flight checkpoint");
	gate.Allow(1);
	gate.WaitFor(gate.entered, 2);
	gate.Allow(2);
	Require(stopped.get(), "shutdown flushes newer generation");
	Require(worker.Stop(), "stop is idempotent");
	Require(gate.entered == 2, "dirty bursts coalesce and clean state is not saved again");
}

void PeriodicAndRetryContracts() {
	std::mutex mutex;
	std::condition_variable condition;
	unsigned calls = 0;
	Worker worker([&] {
		std::lock_guard lock(mutex);
		++calls;
		condition.notify_all();
		if (calls == 1) throw std::runtime_error("extract allocation failure");
		return calls >= 3; // second call models failed extraction/write/rename
	}, 10ms);
	worker.MarkDirty();
	{
		std::unique_lock lock(mutex);
		Require(condition.wait_for(lock, 2s, [&] { return calls >= 3; }), "periodic failure retries");
	}
	Require(worker.Stop(), "successful retry clears dirty generation");
	Require(calls == 3, "no duplicate callback after successful retry");
	unsigned failed_calls = 0;
	Worker failing([&] { ++failed_calls; return false; }, 5min);
	failing.MarkDirty();
	Require(!failing.Stop() && failed_calls == 1, "shutdown failure is bounded and reported");
	Require(!failing.Stop() && failed_calls == 1, "stopped worker does not silently retry");
	unsigned clean_calls = 0;
	Worker clean([&] { ++clean_calls; return true; }, 10ms);
	Require(clean.Stop() && clean_calls == 0, "clean cache never invokes extraction");
}

void StorageDoesNotHoldCacheAccess() {
	// This models the production callback's exact lock split: take a private
	// snapshot under the Vulkan-cache mutex, then write with that mutex released.
	std::mutex cache_mutex;
	std::vector<uint8_t> driver {1};
	SavedPayload saved;
	Gate storage;
	Worker worker([&] {
		std::vector<uint8_t> snapshot;
		{
			std::lock_guard lock(cache_mutex);
			snapshot = driver;
		}
		return saved.Save(std::move(snapshot), [&](auto) { return storage.Callback(); });
	}, 5min);
	worker.MarkDirty();
	worker.Request();
	storage.WaitFor(storage.entered, 1);
	{
		std::unique_lock lock(cache_mutex, std::try_to_lock);
		Require(lock.owns_lock(), "disk work holds no Vulkan cache lock");
		driver = {2};
	}
	worker.MarkDirty();
	worker.Request();
	storage.Allow(1);
	storage.WaitFor(storage.entered, 2);
	storage.Allow(2);
	storage.WaitFor(storage.completed, 2);
	Require(worker.Stop(), "new pipeline during storage remains dirty");
	Require(storage.entered == 2, "both snapshots persisted once");
}

} // namespace

int main() {
	try {
		PayloadContracts();
		CoalescingAndShutdownContracts();
		PeriodicAndRetryContracts();
		StorageDoesNotHoldCacheAccess();
		std::puts("PS5_DRIVER_CACHE_CHECKPOINT_TEST PASS: dedup, coalescing, retry, shutdown, lock split");
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "PS5_DRIVER_CACHE_CHECKPOINT_TEST FAIL: %s\n", error.what());
		return 1;
	}
}
