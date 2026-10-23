#include "Lumen/Core/RefCounterPolicy.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Lumen::Core {
namespace {
std::atomic_bool g_atomic_refs {false};
}

void ConfigureAtomicRefsFromEnvironment() noexcept
{
	const char* value = std::getenv("LUMEN_CORE_ATOMIC_REFS");
	g_atomic_refs.store(value != nullptr && std::strcmp(value, "1") == 0, std::memory_order_release);
}

bool AtomicRefsEnabled() noexcept
{
	return g_atomic_refs.load(std::memory_order_acquire);
}

} // namespace Lumen::Core
