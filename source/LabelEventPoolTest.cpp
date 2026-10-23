#include "Emulator/Graphics/LabelEventPool.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using Lumen::Libs::Graphics::LabelEventPool;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "LabelEventPoolTest: failed line %d\n", __LINE__); return 1; } } while (false)

int main()
{
	LabelEventPool<uintptr_t, uintptr_t> pool;
	uintptr_t event = 999;
	CHECK(!pool.Take(1, &event) && event == 0);
	CHECK(!pool.Take(1, nullptr));
	CHECK(!pool.Recycle(0, 1) && !pool.Recycle(1, 0));
	CHECK(pool.Size() == 0 && pool.KnownSize() == 0);
	CHECK(pool.Recycle(1, 21) && pool.Recycle(2, 21) && pool.Recycle(1, 23));
	CHECK(!pool.Recycle(1, 21) && !pool.Recycle(2, 21));
	CHECK(pool.Size() == 3 && pool.KnownSize() == 3);
	CHECK(pool.Take(1, &event) && event == 23);
	CHECK(pool.Take(2, &event) && event == 21);
	CHECK(!pool.Take(2, &event) && event == 0);
	CHECK(!pool.Take(3, &event) && event == 0);
	CHECK(pool.Take(1, &event) && event == 21);
	CHECK(pool.Size() == 0 && pool.KnownSize() == 3);

	// FinishSessionReset returns all events of detached, completed labels. It
	// does not clear idle native objects or their device identity. The next
	// session can reuse each one once; a repeated return must not publish it twice.
	CHECK(pool.Recycle(1, 23) && pool.Recycle(1, 21) && pool.Recycle(2, 21));
	CHECK(!pool.Recycle(1, 23) && pool.Size() == 3 && pool.KnownSize() == 3);
	CHECK(pool.Take(1, &event) && event == 21);
	CHECK(pool.Take(1, &event) && event == 23);
	CHECK(pool.Take(2, &event) && event == 21);
	CHECK(!pool.Take(1, &event) && !pool.Take(2, &event));

	// A large completed batch shares native numeric handle values between
	// devices. Leave half checked out across repeated rounds: none may be handed
	// out again before its explicit completion/return, even after a reset batch.
	constexpr uintptr_t per_device = 4096;
	LabelEventPool<uintptr_t, uintptr_t> large;
	for (uintptr_t device = 1; device <= 2; ++device)
		for (uintptr_t handle = 1; handle <= per_device; ++handle)
			CHECK(large.Recycle(device, handle));
	std::array<std::vector<uintptr_t>, 2> pending;
	for (uintptr_t device = 1; device <= 2; ++device)
		for (uintptr_t handle = per_device; handle > per_device / 2; --handle)
		{
			CHECK(large.Take(device, &event) && event == handle);
			pending[device - 1].push_back(event);
		}
	CHECK(large.Size() == per_device && large.KnownSize() == 2 * per_device);
	for (unsigned round = 0; round < 64; ++round)
	{
		for (uintptr_t device = 1; device <= 2; ++device)
		{
			std::vector<uintptr_t> returned;
			for (uintptr_t handle = per_device / 2; handle != 0; --handle)
			{
				CHECK(large.Take(device, &event) && event == handle);
				returned.push_back(event);
			}
			CHECK(!large.Take(device, &event) && event == 0);
			for (auto i = returned.rbegin(); i != returned.rend(); ++i)
				CHECK(large.Recycle(device, *i));
			CHECK(!large.Recycle(device, 1));
		}
		CHECK(large.Size() == per_device && large.KnownSize() == 2 * per_device);
	}
	for (uintptr_t device = 1; device <= 2; ++device)
		for (auto i = pending[device - 1].rbegin(); i != pending[device - 1].rend(); ++i)
			CHECK(large.Recycle(device, *i));
	CHECK(large.Size() == 2 * per_device && large.KnownSize() == 2 * per_device);
	for (uintptr_t device = 1; device <= 2; ++device)
	{
		for (uintptr_t handle = per_device; handle != 0; --handle)
			CHECK(large.Take(device, &event) && event == handle);
		CHECK(!large.Take(device, &event) && event == 0);
	}
	CHECK(large.Size() == 0 && large.KnownSize() == 2 * per_device);
	std::puts("LabelEventPoolTest: PASS duplicate/device-isolation/LIFO/reset/pending-ownership/warm-reuse");
	return 0;
}
