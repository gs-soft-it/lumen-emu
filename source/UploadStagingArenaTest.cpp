#include "Emulator/Graphics/UploadStagingArena.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Lumen::Libs::Graphics;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "UploadStagingArenaTest: failed line %d\n", __LINE__); return 1; } } while (false)

int main()
{
	// Pixel and vertex uploads with nonzero offsets must remain distinct until
	// their recording completes. Test arbitrary block alignment, not just pow2.
	UploadStagingCursor cursor {4096, 0};
	std::vector<uint8_t> storage(cursor.capacity, 0xa5);
	std::array<uint64_t, 4> offsets {};
	const std::array<uint64_t, 4> lengths {28, 513, 80, 1024};
	for (size_t i = 0; i < lengths.size(); ++i)
	{
		CHECK(cursor.Allocate(lengths[i], i % 2 == 0 ? 256 : 768, &offsets[i]));
		std::memset(storage.data() + offsets[i], int(i + 1), lengths[i]);
	}
	CHECK(offsets[0] == 0 && offsets[1] == 768 && offsets[2] == 1536 && offsets[3] == 2304);
	for (size_t byte = 0; byte < storage.size(); ++byte)
	{
		uint8_t expected = 0xa5;
		for (size_t i = 0; i < lengths.size(); ++i)
			if (byte >= offsets[i] && byte < offsets[i] + lengths[i]) expected = uint8_t(i + 1);
		CHECK(storage[byte] == expected);
	}
	uint64_t output = 0xbad;
	const auto used = cursor.used;
	CHECK(!cursor.Allocate(1024, 768, &output) && cursor.used == used && output == 0xbad);
	CHECK(!cursor.Allocate(1, 0, &output) && !cursor.Allocate(0, 1, &output));
	CHECK(!cursor.Allocate(1, 1, nullptr));
	UploadStagingCursor overflow {UINT64_MAX, UINT64_MAX - 8};
	CHECK(!overflow.Allocate(9, 1, &output) && !overflow.Allocate(1, UINT64_MAX, &output));
	CHECK(overflow.Allocate(8, 1, &output) && overflow.used == UINT64_MAX);
	CHECK(!overflow.Allocate(1, 1, &output));
	UploadStagingCursor oversized {UploadStagingPoolPolicy::SlabBytes, 0};
	CHECK(!oversized.Allocate(UploadStagingPoolPolicy::SlabBytes + 1, 256, &output));

	// Eight recordings in flight plus eight idle-to-be slabs saturate the
	// bounded pool. No acquire/reset may wrap into their unread bytes.
	UploadStagingPoolPolicy pool;
	std::array<UploadStagingPoolPolicy::Lease, UploadStagingPoolPolicy::MaxSlabs> pending;
	for (auto& lease: pending)
	{
		lease = pool.Reserve(UploadStagingPoolPolicy::SlabBytes);
		CHECK(lease.Valid() && pool.Busy(lease) && !pool.Acquire(lease.slot).Valid());
	}
	CHECK(pool.Bytes() == UploadStagingPoolPolicy::MaxBytes && !pool.Reserve(1).Valid());
	CHECK(!pool.CanReset() && !pool.Reset());
	for (uint32_t i = 0; i < 8; ++i) CHECK(pool.Complete(pending[i]));
	for (uint32_t i = 8; i < 16; ++i) CHECK(pool.Busy(pending[i]));
	CHECK(!pool.Reset());
	const auto reused = pool.Acquire(pending[2].slot);
	CHECK(reused.Valid() && reused.generation != pending[2].generation && pool.Busy(reused));
	CHECK(!pool.Complete(pending[2]) && !pool.CancelReservation(pending[2]));
	CHECK(pool.Complete(reused) && !pool.Complete(reused));
	for (uint32_t i = 8; i < 16; ++i) CHECK(pool.Complete(pending[i]));
	CHECK(pool.Reset() && pool.Bytes() == 0);
	const auto after_reset = pool.Reserve(4096);
	CHECK(after_reset.Valid() && !pool.Complete(pending[after_reset.slot]));
	// Allocation/bind/map failures release reserved native bytes before the
	// caller falls back to a dedicated buffer. Failed reservations are inert.
	CHECK(pool.CancelReservation(after_reset) && !pool.CancelReservation(after_reset));
	CHECK(pool.Bytes() == 0 && pool.CanReset());
	CHECK(!pool.Reserve(UINT64_MAX).Valid() && !pool.Reserve(0).Valid());
	const auto all_budget = pool.Reserve(UploadStagingPoolPolicy::MaxBytes);
	CHECK(all_budget.Valid() && !pool.Reserve(1).Valid());
	CHECK(pool.Complete(all_budget) && pool.RemoveAvailable(all_budget.slot));
	CHECK(pool.Bytes() == 0 && !pool.RemoveAvailable(all_budget.slot));
	std::puts("UploadStagingArenaTest: PASS disjoint-slices/padding/alignment in-flight/no-wrap budget allocation-failure generation/ABA reset");
	return 0;
}
