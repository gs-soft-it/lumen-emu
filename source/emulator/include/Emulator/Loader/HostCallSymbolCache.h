#ifndef EMULATOR_LOADER_HOSTCALLSYMBOLCACHE_H_
#define EMULATOR_LOADER_HOSTCALLSYMBOLCACHE_H_

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Lumen::Loader {

// Native targets and the resolver's symbol storage must remain valid for this
// cache's lifetime. Entries never move or change after publication. This is a
// diagnostic cache, not an authority for admitting or reading guest memory.
template <size_t Capacity> class HostCallSymbolCache final
{
public:
	static_assert(Capacity != 0);

	template <typename Resolver> const char* Lookup(bool enabled, uint64_t target, Resolver&& resolver)
	{
		if (!enabled) return nullptr;
		const ErrnoRestore preserve_errno;
		if (target == 0) return "?";
		const size_t first = (target >> 4) % Capacity;
		if (const char* symbol = Find(target, first)) return symbol;

		std::unique_lock lock(m_mutex);
		// A competing miss may have published this target before we acquired the
		// lock. Resolve each resident target once, including unsuccessful lookup.
		for (size_t probe = 0; probe < Capacity; ++probe)
		{
			auto& entry = m_entries[(first + probe) % Capacity];
			const uint64_t cached_target = entry.target.load(std::memory_order_acquire);
			if (cached_target == target) return entry.symbol;
			if (cached_target != 0) continue;
			const char* symbol = resolver(target);
			entry.symbol = symbol != nullptr ? symbol : "?";
			entry.target.store(target, std::memory_order_release);
			return entry.symbol;
		}
		// Keep exact diagnostic names when the fixed table is full. This path
		// does not retain another entry and cannot evict a name used by a trace.
		lock.unlock();
		const char* symbol = resolver(target);
		return symbol != nullptr ? symbol : "?";
	}

private:
	struct ErrnoRestore
	{
		int saved = errno;
		~ErrnoRestore() { errno = saved; }
	};
	struct Entry
	{
		std::atomic<uint64_t> target {0};
		const char* symbol = nullptr;
	};
	const char* Find(uint64_t target, size_t first) const
	{
		for (size_t probe = 0; probe < Capacity; ++probe)
		{
			const auto& entry = m_entries[(first + probe) % Capacity];
			const uint64_t cached_target = entry.target.load(std::memory_order_acquire);
			if (cached_target == target) return entry.symbol;
			if (cached_target == 0) return nullptr;
		}
		return nullptr;
	}
	std::array<Entry, Capacity> m_entries {};
	std::mutex m_mutex;
};

} // namespace Lumen::Loader

#endif // EMULATOR_LOADER_HOSTCALLSYMBOLCACHE_H_
