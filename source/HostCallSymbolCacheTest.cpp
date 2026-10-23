#include "Emulator/Loader/HostCallSymbolCache.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "HostCallSymbolCacheTest: %s\n", message);
	return condition;
}

bool BoundedCollisionAndErrno()
{
	Lumen::Loader::HostCallSymbolCache<2> cache;
	std::array<unsigned, 4> calls {};
	const auto resolve = [&](uint64_t target) -> const char*
	{
		errno = EIO;
		++calls[target / 32 - 1];
		return target == 32 ? "first" : target == 64 ? "second" : nullptr;
	};
	errno = EDOM;
	bool ok = Check(cache.Lookup(false, 32, resolve) == nullptr && calls[0] == 0 && errno == EDOM,
	                "disabled trace performed a lookup or changed errno");
	ok = Check(std::strcmp(cache.Lookup(true, 32, resolve), "first") == 0 && errno == EDOM,
	           "first miss changed name or errno") && ok;
	// Both native-address candidates hash to bucket zero in the two-entry table.
	ok = Check(std::strcmp(cache.Lookup(true, 64, resolve), "second") == 0 && errno == EDOM,
	           "collision lost its distinct name") && ok;
	for (unsigned iteration = 0; iteration < 100; ++iteration)
	{
		ok = Check(std::strcmp(cache.Lookup(true, 32, resolve), "first") == 0 &&
		           std::strcmp(cache.Lookup(true, 64, resolve), "second") == 0 && errno == EDOM,
		           "resident lookup changed a name or errno") && ok;
	}
	ok = Check(calls[0] == 1 && calls[1] == 1, "resident targets were resolved again") && ok;
	ok = Check(std::strcmp(cache.Lookup(true, 96, resolve), "?") == 0 &&
	           std::strcmp(cache.Lookup(true, 96, resolve), "?") == 0 && calls[2] == 2 && errno == EDOM,
	           "full table grew, evicted a resident, or changed errno") && ok;
	ok = Check(std::strcmp(cache.Lookup(true, 32, resolve), "first") == 0 && calls[0] == 1,
	           "full fallback evicted a resident") && ok;
	ok = Check(std::strcmp(cache.Lookup(true, 0, resolve), "?") == 0 && errno == EDOM,
	           "zero target reached the resolver or changed errno") && ok;
	return ok;
}

bool ConcurrentPublicationAndUnknown()
{
	Lumen::Loader::HostCallSymbolCache<4> cache;
	std::atomic<unsigned> calls {0};
	std::atomic<bool> start {false}, ok {true};
	std::array<std::thread, 8> workers;
	for (auto& worker: workers)
	{
		worker = std::thread([&]
		{
			errno = ERANGE;
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (unsigned iteration = 0; iteration < 1000; ++iteration)
			{
				const char* symbol = cache.Lookup(true, 32, [&](uint64_t target) -> const char*
				{
					if (target != 32) ok = false;
					++calls;
					errno = EIO;
					std::this_thread::yield();
					return "published";
				});
				if (std::strcmp(symbol, "published") != 0 || errno != ERANGE) ok = false;
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();
	unsigned unknown_calls = 0;
	const auto unknown = [&](uint64_t) -> const char* { ++unknown_calls; errno = EIO; return nullptr; };
	errno = EDOM;
	const bool unknown_ok = std::strcmp(cache.Lookup(true, 64, unknown), "?") == 0 &&
	                        std::strcmp(cache.Lookup(true, 64, unknown), "?") == 0 && unknown_calls == 1 && errno == EDOM;
	return Check(ok && calls == 1, "concurrent miss was not published once with per-thread errno intact") &&
	       Check(unknown_ok, "unresolved resident target was not cached or changed errno");
}

} // namespace

int main()
{
	if (!BoundedCollisionAndErrno() || !ConcurrentPublicationAndUnknown()) return 1;
	std::puts("HostCallSymbolCacheTest: PASS (disabled/collision/bound/unknown/concurrency/errno)");
	return 0;
}
