// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DRAWACCESSRESERVATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DRAWACCESSRESERVATION_H_

#include <cstdint>
#include <limits>

namespace Lumen::Libs::Graphics {

class CommandProcessor;
struct VulkanCommandPool;

struct GpuMemoryRecordingIdentity
{
	CommandProcessor*  cp                            = nullptr;
	VulkanCommandPool* pool                          = nullptr;
	uint64_t           recording_id                  = 0;
	uint64_t           command_ordinal               = 0;
	uint64_t           submission_session_epoch      = 0;
	uint64_t           recording_submit_serial       = 0;
	uint64_t           begin_submit_serial_cutoff    = 0;
	uint64_t           begin_completed_serial_cutoff = 0;
	int                queue                         = -1;
};

// A synchronous, processor-serialized ticket owned by one CommandBuffer.
// This records no accesses, publishes no producer and advances no ordinal.
// The caller retains its exclusive admission, resource proofs and native
// ownership throughout; the ticket does not provide any of those authorities.
// Every rejected transition is terminal. Submission/reset/destruction must
// reject an active or failed ticket, including same-thread callback reentry.
class DrawAccessReservation
{
public:
	enum class State { Idle, PendingNative, Committing, Committed, Failed };

	DrawAccessReservation() = default;
	DrawAccessReservation(const DrawAccessReservation&) = delete;
	DrawAccessReservation& operator=(const DrawAccessReservation&) = delete;
	DrawAccessReservation(DrawAccessReservation&&) = delete;
	DrawAccessReservation& operator=(DrawAccessReservation&&) = delete;

	[[nodiscard]] bool Reserve(const GpuMemoryRecordingIdentity& identity, uint32_t slot, const void* thread,
	                           uint64_t expected_ordinal_count, uint64_t* token_out) noexcept
	{
		if (token_out != nullptr) *token_out = 0;
		if ((m_state != State::Idle && m_state != State::Committed) || token_out == nullptr ||
		    !IdentityWellFormed(identity) || slot == std::numeric_limits<uint32_t>::max() || thread == nullptr ||
		    expected_ordinal_count > std::numeric_limits<uint64_t>::max() - identity.command_ordinal ||
		    m_token == std::numeric_limits<uint64_t>::max()) return Reject();
		m_identity = identity;
		m_slot = slot;
		m_thread = thread;
		m_end_ordinal = identity.command_ordinal + expected_ordinal_count;
		++m_token; // Never zero, reset or wrapped during this controller's lifetime.
		m_state = State::PendingNative;
		*token_out = m_token;
		return true;
	}

	[[nodiscard]] bool BeginCommit(uint64_t token, const GpuMemoryRecordingIdentity& current, uint32_t slot,
	                               const void* thread) noexcept
	{
		if (m_state != State::PendingNative || !Matches(token, current, slot, thread, m_identity.command_ordinal))
			return Reject();
		m_state = State::Committing;
		return true;
	}

	[[nodiscard]] bool Finish(uint64_t token, const GpuMemoryRecordingIdentity& current, uint32_t slot,
	                          const void* thread) noexcept
	{
		if (m_state != State::Committing || !Matches(token, current, slot, thread, m_end_ordinal)) return Reject();
		m_state = State::Committed;
		return true;
	}

	[[nodiscard]] State GetState() const noexcept { return m_state; }
	[[nodiscard]] bool Active() const noexcept { return m_state == State::PendingNative || m_state == State::Committing; }
	[[nodiscard]] bool Failed() const noexcept { return m_state == State::Failed; }
	[[nodiscard]] bool BlocksSubmission() const noexcept { return Active() || Failed(); }

private:
	static bool IdentityWellFormed(const GpuMemoryRecordingIdentity& identity) noexcept
	{
		// The caller additionally validates the native queue domain. These are
		// the recording/submission invariants independent of a graphics device.
		return identity.cp != nullptr && identity.pool != nullptr && identity.recording_id != 0 && identity.queue >= 0 &&
		       identity.submission_session_epoch != 0 && identity.recording_submit_serial != 0 &&
		       identity.begin_submit_serial_cutoff != std::numeric_limits<uint64_t>::max() &&
		       identity.recording_submit_serial == identity.begin_submit_serial_cutoff + 1 &&
		       identity.begin_completed_serial_cutoff <= identity.begin_submit_serial_cutoff;
	}

	bool Matches(uint64_t token, const GpuMemoryRecordingIdentity& current, uint32_t slot, const void* thread,
	             uint64_t ordinal) const noexcept
	{
		return token != 0 && token == m_token && slot == m_slot && thread == m_thread &&
		       current.cp == m_identity.cp && current.pool == m_identity.pool &&
		       current.recording_id == m_identity.recording_id && current.command_ordinal == ordinal &&
		       current.submission_session_epoch == m_identity.submission_session_epoch &&
		       current.recording_submit_serial == m_identity.recording_submit_serial &&
		       current.begin_submit_serial_cutoff == m_identity.begin_submit_serial_cutoff &&
		       current.begin_completed_serial_cutoff == m_identity.begin_completed_serial_cutoff &&
		       current.queue == m_identity.queue;
	}

	bool Reject() noexcept { m_state = State::Failed; return false; }

	GpuMemoryRecordingIdentity m_identity {};
	const void* m_thread = nullptr;
	uint64_t m_token = 0;
	uint64_t m_end_ordinal = 0;
	uint32_t m_slot = std::numeric_limits<uint32_t>::max();
	State m_state = State::Idle;
};

} // namespace Lumen::Libs::Graphics

#endif
