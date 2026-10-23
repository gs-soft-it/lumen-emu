// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Graphics/DrawAccessReservation.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

using namespace Lumen::Libs::Graphics;

static_assert(!std::is_copy_constructible_v<DrawAccessReservation>);
static_assert(!std::is_copy_assignable_v<DrawAccessReservation>);
static_assert(!std::is_move_constructible_v<DrawAccessReservation>);
static_assert(!std::is_move_assignable_v<DrawAccessReservation>);
static_assert(std::is_trivially_destructible_v<DrawAccessReservation>);

namespace {

uint32_t checks = 0;
int processor_storage[2] {};
int pool_storage[2] {};
int thread_storage[2] {};
constexpr uint32_t slot = 3;
constexpr uint64_t max_ordinal = std::numeric_limits<uint64_t>::max();

void Require(bool condition, const char* label)
{
	++checks;
	if (!condition) { std::fprintf(stderr, "DrawAccessReservationTest: FAIL %s\n", label); std::abort(); }
}

GpuMemoryRecordingIdentity Identity(uint64_t ordinal = 11)
{
	GpuMemoryRecordingIdentity identity {};
	identity.cp = reinterpret_cast<CommandProcessor*>(&processor_storage[0]);
	identity.pool = reinterpret_cast<VulkanCommandPool*>(&pool_storage[0]);
	identity.recording_id = 41;
	identity.command_ordinal = ordinal;
	identity.submission_session_epoch = 7;
	identity.recording_submit_serial = 13;
	identity.begin_submit_serial_cutoff = 12;
	identity.begin_completed_serial_cutoff = 9;
	identity.queue = 0;
	return identity;
}

void RequireFailed(DrawAccessReservation& reservation)
{
	Require(reservation.GetState() == DrawAccessReservation::State::Failed && reservation.Failed() &&
	        !reservation.Active() && reservation.BlocksSubmission(), "failure blocks submission");
	uint64_t replacement_token = 999;
	Require(!reservation.Reserve(Identity(), slot, &thread_storage[0], 0, &replacement_token) && replacement_token == 0,
	        "failed controller cannot reserve or expose a token");
	Require(!reservation.BeginCommit(1, Identity(), slot, &thread_storage[0]), "failed controller cannot begin commit");
	Require(!reservation.Finish(1, Identity(), slot, &thread_storage[0]), "failed controller cannot finish");
	Require(reservation.Failed() && reservation.BlocksSubmission(), "failure stays latched");
}

uint64_t Reserve(DrawAccessReservation& reservation, const GpuMemoryRecordingIdentity& identity, uint64_t count = 2)
{
	uint64_t token = 0;
	Require(reservation.Reserve(identity, slot, &thread_storage[0], count, &token) && token != 0, "reserve exact recording");
	Require(reservation.GetState() == DrawAccessReservation::State::PendingNative && reservation.Active() &&
	        !reservation.Failed() && reservation.BlocksSubmission(), "pending native blocks submission");
	return token;
}

void SuccessfulTransitions()
{
	for (const auto& range: std::array<std::array<uint64_t, 2>, 6> {{{0, 0}, {11, 0}, {11, 7},
	                                                               {0, max_ordinal}, {max_ordinal - 1, 1}, {max_ordinal, 0}}})
	{
		DrawAccessReservation reservation;
		Require(reservation.GetState() == DrawAccessReservation::State::Idle && !reservation.Active() &&
		        !reservation.Failed() && !reservation.BlocksSubmission(), "idle permits submission");
		auto identity = Identity(range[0]);
		const auto token = Reserve(reservation, identity, range[1]);
		Require(identity.command_ordinal == range[0], "reserve has no ordinal side effect");
		Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "begin at exact captured ordinal");
		Require(reservation.GetState() == DrawAccessReservation::State::Committing && reservation.Active() &&
		        reservation.BlocksSubmission(), "committing blocks reentrant submission");
		Require(identity.command_ordinal == range[0], "begin commit has no ordinal side effect");
		identity.command_ordinal += range[1]; // Model only the declared legacy effects.
		Require(reservation.Finish(token, identity, slot, &thread_storage[0]), "finish at exact declared ordinal count");
		Require(identity.command_ordinal == range[0] + range[1], "finish has no ordinal side effect");
		Require(reservation.GetState() == DrawAccessReservation::State::Committed && !reservation.Active() &&
		        !reservation.Failed() && !reservation.BlocksSubmission(), "committed permits submission");
	}

	DrawAccessReservation reservation;
	uint64_t previous_token = 0;
	for (uint64_t recording = 1; recording <= 3; ++recording)
	{
		auto identity = Identity(recording);
		identity.recording_id += recording;
		const auto token = Reserve(reservation, identity, 0);
		Require(token > previous_token, "committed reuse receives a strictly newer nonzero token");
		Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]) &&
		        reservation.Finish(token, identity, slot, &thread_storage[0]), "zero-effect draw completes explicitly");
		previous_token = token;
	}
}

void InvalidReservations()
{
	for (uint32_t field = 0; field < 10; ++field)
	{
		auto identity = Identity();
		switch (field)
		{
			case 0: identity.cp = nullptr; break;
			case 1: identity.pool = nullptr; break;
			case 2: identity.recording_id = 0; break;
			case 3: identity.queue = -1; break;
			case 4: identity.submission_session_epoch = 0; break;
			case 5: identity.recording_submit_serial = 0; break;
			case 6: identity.begin_submit_serial_cutoff = max_ordinal; break;
			case 7: ++identity.recording_submit_serial; break;
			case 8: identity.begin_completed_serial_cutoff = identity.begin_submit_serial_cutoff + 1; break;
			case 9: identity.command_ordinal = max_ordinal; break;
		}
		DrawAccessReservation reservation;
		uint64_t token = 99;
		Require(!reservation.Reserve(identity, slot, &thread_storage[0], 1, &token) && token == 0,
		        "malformed identity or overflowing ordinal denied before reservation");
		RequireFailed(reservation);
	}
	for (uint32_t condition = 0; condition < 4; ++condition)
	{
		DrawAccessReservation reservation;
		uint64_t token = 99;
		const auto identity = Identity(condition == 3 ? max_ordinal - 1 : 11);
		Require(!reservation.Reserve(identity, condition == 0 ? std::numeric_limits<uint32_t>::max() : slot,
		                            condition == 1 ? nullptr : &thread_storage[0], 2, condition == 2 ? nullptr : &token),
		        "missing slot/thread/output or ordinal overflow denied");
		if (condition != 2) Require(token == 0, "failed reserve clears output token");
		RequireFailed(reservation);
	}
}

void MutateIdentity(GpuMemoryRecordingIdentity* identity, uint32_t field)
{
	switch (field)
	{
		case 0: identity->cp = reinterpret_cast<CommandProcessor*>(&processor_storage[1]); break;
		case 1: identity->pool = reinterpret_cast<VulkanCommandPool*>(&pool_storage[1]); break;
		case 2: ++identity->recording_id; break;
		case 3: ++identity->command_ordinal; break;
		case 4: ++identity->submission_session_epoch; break;
		case 5: ++identity->recording_submit_serial; break;
		case 6: ++identity->begin_submit_serial_cutoff; break;
		case 7: ++identity->begin_completed_serial_cutoff; break;
		case 8: ++identity->queue; break;
	}
}

void IdentityAndAuthorityMismatch()
{
	for (uint32_t phase = 0; phase < 2; ++phase)
	{
		for (uint32_t field = 0; field < 9; ++field)
		{
			DrawAccessReservation reservation;
			auto identity = Identity();
			const auto token = Reserve(reservation, identity);
			if (phase == 1)
			{
				Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "begin before finish mismatch");
				identity.command_ordinal += 2;
			}
			MutateIdentity(&identity, field);
			Require(!(phase == 0 ? reservation.BeginCommit(token, identity, slot, &thread_storage[0]) :
			                       reservation.Finish(token, identity, slot, &thread_storage[0])),
			        "every recording identity component must match exactly");
			RequireFailed(reservation);
		}
		for (uint32_t authority = 0; authority < 5; ++authority)
		{
			DrawAccessReservation reservation;
			auto identity = Identity();
			const auto token = Reserve(reservation, identity);
			if (phase == 1)
			{
				Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "begin before authority mismatch");
				identity.command_ordinal += 2;
			}
			const auto offered_token = authority == 0 ? 0 : (authority == 1 ? token + 1 : token);
			const auto offered_slot = authority == 2 ? slot + 1 : slot;
			const void* offered_thread = authority == 3 ? &thread_storage[1] :
			                             (authority == 4 ? nullptr : &thread_storage[0]);
			Require(!(phase == 0 ? reservation.BeginCommit(offered_token, identity, offered_slot, offered_thread) :
			                       reservation.Finish(offered_token, identity, offered_slot, offered_thread)),
			        "wrong token/slot/thread cannot advance a ticket");
			RequireFailed(reservation);
		}
	}
	DrawAccessReservation reservation;
	auto identity = Identity();
	const auto token = Reserve(reservation, identity);
	Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "begin undercount test");
	++identity.command_ordinal;
	Require(!reservation.Finish(token, identity, slot, &thread_storage[0]), "missing declared effect cannot finish");
	RequireFailed(reservation);
}

void InvalidTransitions()
{
	for (uint32_t transition = 0; transition < 8; ++transition)
	{
		DrawAccessReservation reservation;
		auto identity = Identity();
		if (transition < 2)
		{
			Require(!(transition == 0 ? reservation.BeginCommit(1, identity, slot, &thread_storage[0]) :
			                           reservation.Finish(1, identity, slot, &thread_storage[0])),
			        "idle cannot commit or finish");
		} else
		{
			const auto token = Reserve(reservation, identity, 0);
			if (transition == 2)
			{
				uint64_t nested = 999;
				Require(!reservation.Reserve(identity, slot, &thread_storage[0], 0, &nested) && nested == 0,
				        "nested pending reservation fails closed");
			} else if (transition == 3)
				Require(!reservation.Finish(token, identity, slot, &thread_storage[0]), "pending cannot skip begin commit");
			else
			{
				Require(reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "valid begin for transition test");
				if (transition == 4)
				{
					uint64_t nested = 999;
					Require(!reservation.Reserve(identity, slot, &thread_storage[0], 0, &nested) && nested == 0,
					        "nested committing reservation fails closed");
				} else if (transition == 5)
					Require(!reservation.BeginCommit(token, identity, slot, &thread_storage[0]), "double begin commit fails closed");
				else
				{
					Require(reservation.Finish(token, identity, slot, &thread_storage[0]), "valid finish for transition test");
					Require(!(transition == 6 ? reservation.Finish(token, identity, slot, &thread_storage[0]) :
					                           reservation.BeginCommit(token, identity, slot, &thread_storage[0])),
					        "committed ticket cannot finish or begin again");
				}
			}
		}
		RequireFailed(reservation);
	}

	for (uint32_t phase = 0; phase < 2; ++phase)
	{
		DrawAccessReservation reservation;
		const auto identity = Identity();
		const auto old_token = Reserve(reservation, identity, 0);
		Require(reservation.BeginCommit(old_token, identity, slot, &thread_storage[0]) &&
		        reservation.Finish(old_token, identity, slot, &thread_storage[0]), "finish old ticket before identical-identity reuse");
		const auto new_token = Reserve(reservation, identity, 0);
		Require(new_token > old_token, "same identity never recycles a ticket token");
		if (phase == 1)
			Require(reservation.BeginCommit(new_token, identity, slot, &thread_storage[0]), "begin newer ticket before stale finish");
		Require(!(phase == 0 ? reservation.BeginCommit(old_token, identity, slot, &thread_storage[0]) :
		                       reservation.Finish(old_token, identity, slot, &thread_storage[0])),
		        "stale ticket cannot commit even when every identity field is unchanged");
		RequireFailed(reservation);
	}
}

} // namespace

int main()
{
	SuccessfulTransitions();
	InvalidReservations();
	IdentityAndAuthorityMismatch();
	InvalidTransitions();
	std::printf("DrawAccessReservationTest: PASS %u checks; identity ordinal zero-effects overflow reentry stale-token fail-closed\n", checks);
	return 0;
}
