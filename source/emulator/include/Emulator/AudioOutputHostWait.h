#pragma once

#include <utility>

namespace Lumen::Libs::Audio::Host {

// Construct only after all guest inputs have been copied and their leases
// released. Native port work can then wait without retaining a guest reader.
// Member order is essential: every submit lock is released before readmission,
// including on errors, so a peer HLE holding a reader can finish on that port.
template <class HostWait, class SubmitLocks>
class OutputHostWait final
{
public:
	template <class... Args>
	explicit OutputHostWait(Args&&... args): m_host_wait(std::forward<Args>(args)...) {}

	OutputHostWait(const OutputHostWait&) = delete;
	OutputHostWait& operator=(const OutputHostWait&) = delete;

	SubmitLocks& Locks() { return m_submit_locks; }

private:
	HostWait    m_host_wait;
	SubmitLocks m_submit_locks;
};

} // namespace Lumen::Libs::Audio::Host
