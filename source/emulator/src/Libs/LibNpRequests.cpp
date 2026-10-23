#include "Emulator/Libs/NpRequests.h"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::NpRequests {

namespace {

std::mutex g_mutex;
std::unordered_set<int32_t> g_requests;

} // namespace

bool Enabled(Service service)
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	// Snapshot both independent selectors together. Request creation must not
	// leave a second service's environment value unlatched until a later call.
	static const unsigned enabled = [] {
		const auto selected = [](const char* name) {
			const char* value = std::getenv(name);
			return value != nullptr && std::strcmp(value, "1") == 0;
		};
		return (selected("LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE") ? 1u : 0u) |
		       (selected("LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE") ? 2u : 0u);
	}();
	return (enabled & (service == Service::Plus ? 1u : 2u)) != 0;
#else
	(void)service;
	return false;
#endif
}

bool TrackingEnabled()
{
	return Enabled(Service::Plus) || Enabled(Service::Parental);
}

void RecordCreated(int32_t request_id)
{
	(void)TrackingEnabled(); // Preserve the existing first-access selector snapshot.
	if (request_id <= 0) return;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_requests.insert(request_id);
}

void RecordDeleted(int32_t request_id)
{
	(void)TrackingEnabled();
	std::lock_guard<std::mutex> lock(g_mutex);
	g_requests.erase(request_id);
}

void ResetSessionState()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_requests.clear();
}

bool QuiesceRequest(int32_t request_id)
{
	// Guarded offline queries finish before releasing this mutex. Pending async
	// work would require extending this model before reporting quiescence.
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_requests.count(request_id) != 0;
}

LiveRequestGuard::LiveRequestGuard(int32_t request_id):
	m_lock(g_mutex), m_live(TrackingEnabled() && g_requests.count(request_id) != 0) {}

} // namespace Lumen::Libs::NpRequests

#endif // LUMEN_EMU_ENABLED
