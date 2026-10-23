#ifndef LUMEN_EMULATOR_LIBS_NP_REQUESTS_H_
#define LUMEN_EMULATOR_LIBS_NP_REQUESTS_H_

#include "Emulator/Common.h"

#include <cstdint>
#include <mutex>

namespace Lumen::Libs::NpRequests {

enum class Service { Plus, Parental };

[[nodiscard]] bool Enabled(Service service);
[[nodiscard]] bool TrackingEnabled();
void RecordCreated(int32_t request_id);
void RecordDeleted(int32_t request_id);
void ResetSessionState();

// Local NP operations retain no asynchronous job. Serialize with any query
// holding the publication guard, and require an existing request. This neither
// deletes the request nor cancels an external service; later queries may reuse it.
[[nodiscard]] bool QuiesceRequest(int32_t request_id);

// Keep liveness validation and result publication within the same lifetime.
// The caller must not acquire the Network mutex while holding this guard.
// Admission remains restricted to the existing offline experiments.
class LiveRequestGuard
{
public:
	explicit LiveRequestGuard(int32_t request_id);
	LiveRequestGuard(const LiveRequestGuard&) = delete;
	LiveRequestGuard& operator=(const LiveRequestGuard&) = delete;
	[[nodiscard]] bool IsLive() const { return m_live; }

private:
	std::unique_lock<std::mutex> m_lock;
	bool m_live;
};

} // namespace Lumen::Libs::NpRequests

#endif // LUMEN_EMULATOR_LIBS_NP_REQUESTS_H_
