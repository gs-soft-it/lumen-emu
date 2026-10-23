// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Libs::Graphics::DriverCacheCheckpoint {

// Owned by the worker after startup. Compare bytes, not only a hash: a collision
// must never suppress a changed driver cache. Failed writes keep the old state.
class SavedPayload {
public:
	explicit SavedPayload(std::vector<uint8_t> loaded = {}) : m_saved(std::move(loaded)) {}

	template <typename Write>
	bool Save(std::vector<uint8_t> payload, Write&& write) {
		if (payload == m_saved) {
			return true;
		}
		if (!write(std::span<const uint8_t>(payload))) {
			return false;
		}
		m_saved = std::move(payload);
		return true;
	}

private:
	std::vector<uint8_t> m_saved;
};

// MarkDirty never waits for Vulkan or disk work. Only the worker calls the
// callback, and it holds neither its wait mutex nor any renderer mutex then.
// A failed/throwing callback remains pending until the next interval/request.
class Worker {
public:
	explicit Worker(std::function<bool()> checkpoint,
	                std::chrono::steady_clock::duration interval = std::chrono::minutes {5})
	    : m_checkpoint(std::move(checkpoint)), m_interval(interval),
	      m_thread([this] { Run(); }) {}
	~Worker() { (void)Stop(); }
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	void MarkDirty() { m_generation.fetch_add(1, std::memory_order_release); }

	// Explicit Save requests are asynchronous too, including WindowRun's call
	// while holding the renderer mutex. Destruction performs the final join.
	void Request() {
		{
			std::lock_guard lock(m_wait_mutex);
			m_requested = true;
		}
		m_condition.notify_one();
	}

	// The owner must stop producers before shutdown and keep callback resources
	// alive until this returns. Failure is reported without spinning at shutdown.
	bool Stop() {
		std::lock_guard stop_lock(m_stop_mutex);
		if (m_thread.joinable()) {
			{
				std::lock_guard lock(m_wait_mutex);
				m_stopping = true;
			}
			m_condition.notify_one();
			m_thread.join();
		}
		return m_completed_generation == m_generation.load(std::memory_order_acquire);
	}

private:
	void CheckpointPending() noexcept {
		const auto generation = m_generation.load(std::memory_order_acquire);
		if (generation == m_completed_generation) {
			return;
		}
		try {
			if (m_checkpoint()) {
				m_completed_generation = generation;
			}
		} catch (...) {
			// Cache persistence is optional. Leave the generation dirty for retry.
		}
	}

	void Run() {
		for (;;) {
			bool stopping;
			{
				std::unique_lock lock(m_wait_mutex);
				m_condition.wait_for(lock, m_interval, [this] { return m_stopping || m_requested; });
				stopping = m_stopping;
				m_requested = false;
			}
			CheckpointPending();
			if (stopping) {
				return;
			}
		}
	}

	std::function<bool()> m_checkpoint;
	const std::chrono::steady_clock::duration m_interval;
	std::atomic<uint64_t> m_generation {};
	uint64_t m_completed_generation = 0;
	std::mutex m_wait_mutex;
	std::mutex m_stop_mutex;
	std::condition_variable m_condition;
	bool m_requested = false;
	bool m_stopping = false;
	std::thread m_thread;
};

} // namespace Libs::Graphics::DriverCacheCheckpoint
