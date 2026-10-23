#ifndef INCLUDE_LUMEN_CORE_MEMORYINITGATE_H_
#define INCLUDE_LUMEN_CORE_MEMORYINITGATE_H_

#include <atomic>
#include <thread>

namespace Lumen::Core::MemoryDetail {

// Allocator bootstrap cannot allocate or reenter an allocating once primitive.
// The caller supplies a thread-local marker reserved for this gate. Recursive
// entry on that thread may use state that its initializer already constructed;
// every other thread observes only the completed release publication.
class InitGate
{
public:
	constexpr InitGate() noexcept = default;

	bool Ready() const noexcept { return m_ready.load(std::memory_order_acquire); }

	template <typename Initializer>
	void Ensure(bool& recursive_owner, Initializer&& initialize) noexcept
	{
		if (Ready() || recursive_owner) return;
		while (m_claimed.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
		if (!m_ready.load(std::memory_order_relaxed))
		{
			recursive_owner = true;
			initialize();
			recursive_owner = false;
			m_ready.store(true, std::memory_order_release);
		}
		m_claimed.clear(std::memory_order_release);
	}

private:
	std::atomic_bool m_ready {false};
	std::atomic_flag m_claimed = ATOMIC_FLAG_INIT;
};

} // namespace Lumen::Core::MemoryDetail

#endif
