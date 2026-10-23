// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
// Included in pthread.cpp so the contract can exercise the same internal pin
// used by the three public mutex calls without exporting native handles.
bool TestMutexResolvePinLifetime() {
	PthreadMutex object = nullptr;
	if (PthreadMutexInit(&object, nullptr, nullptr) != OK) return false;
	const auto original = object;
	PthreadMutex copied = object;
	bool ready = false, release = false, pinned = false;
	std::mutex control;
	std::condition_variable changed;
	std::thread holder([&] {
		MutexCallPin pin(&copied);
		std::unique_lock lock(control);
		pinned = pin.Get() == original;
		ready = true;
		changed.notify_one();
		changed.wait(lock, [&] { return release; });
	});
	{
		std::unique_lock lock(control);
		changed.wait(lock, [&] { return ready; });
	}
	const auto busy = PthreadMutexDestroy(&object);
	bool pass = pinned && busy == KERNEL_ERROR_EBUSY && object == original;
	{
		std::lock_guard lock(control);
		release = true;
	}
	changed.notify_one();
	holder.join();
	if (object != nullptr) pass = PthreadMutexDestroy(&object) == OK && pass;
	{
		MutexCallPin stale(&copied);
		pass = stale.Get() == nullptr && pass;
	}
	std::printf("PS5_MUTEX_PIN_TEST case=retained-pin-denies-peer-destroy enabled=%u result=%s\n",
	    MutexResolvePinEnabled() ? 1u : 0u, pass ? "PASS" : "FAIL");

	// The copied slot never becomes the lazy null initializer when the other
	// thread destroys the original slot. Resolution either pins the old live
	// object (destroy must be busy), or rejects its retired identity. Hold any
	// successful pin until the destroy result is known, making both outcomes
	// directly checkable without timing guesses or permitting address reuse.
	unsigned resolved = 0, retired = 0;
	for (unsigned attempt = 0; attempt != 32; ++attempt) {
		object = nullptr;
		if (PthreadMutexInit(&object, nullptr, nullptr) != OK) return false;
		const auto identity = object;
		copied = object;
		std::atomic<bool> start {false}, destroy_done {false};
		bool acquired = false;
		int destroyed = KERNEL_ERROR_EINVAL;
		std::thread resolver([&] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			MutexCallPin pin(&copied);
			acquired = pin.Get() == identity;
			while (!destroy_done.load(std::memory_order_acquire)) std::this_thread::yield();
		});
		std::thread destroyer([&] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			destroyed = PthreadMutexDestroy(&object);
			destroy_done.store(true, std::memory_order_release);
		});
		start.store(true, std::memory_order_release);
		resolver.join();
		destroyer.join();
		if (destroyed == KERNEL_ERROR_EBUSY) {
			pass = acquired && object == identity && pass;
			++resolved;
		} else {
			pass = destroyed == OK && !acquired && object == nullptr && pass;
			++retired;
		}
		if (object != nullptr) pass = PthreadMutexDestroy(&object) == OK && pass;
		MutexCallPin stale(&copied);
		pass = stale.Get() == nullptr && pass;
	}
	std::printf("PS5_MUTEX_PIN_TEST case=resolve-destroy-race enabled=%u attempts=32 pinned=%u retired=%u result=%s\n",
	    MutexResolvePinEnabled() ? 1u : 0u, resolved, retired, pass ? "PASS" : "FAIL");
	return pass;
}
