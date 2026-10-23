#include "Emulator/Loader/PrelinkedPlt.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition, const char* name)
{
	if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}

std::array<uint8_t, 256> Fixture(const std::vector<uint64_t>& slots)
{
	std::array<uint8_t, 256> bytes {};
	bytes.fill(0xcc);
	for (size_t i = 0; i < slots.size(); ++i)
	{
		const size_t at = 32 + 16 * i;
		bytes[at] = 0xff; bytes[at + 1] = 0x25;
		int32_t disp = 0;
		Require(Lumen::Loader::PrelinkedPlt::Displacement(0x1000 + at + 6, slots[i], &disp), "fixture displacement");
		std::memcpy(bytes.data() + at + 2, &disp, 4);
	}
	const int32_t external = -0x4000;
	bytes[64] = 0xe9; bytes[69] = 0x90;
	std::memcpy(bytes.data() + 65, &external, 4);
	return bytes;
}
}

int main()
{
	using namespace Lumen::Loader::PrelinkedPlt;
	const std::vector<uint64_t> slots {0x3000, 0x3008, 0x3010, 0x3018, 0x3020, 0x3028};
	auto bytes = Fixture(slots);
	const auto find = [&](const auto& input) { return Find(input.data(), input.size(), 0x1000, 0x4000, slots); };
	auto plan = find(bytes);
	Require(!plan.ambiguous && plan.repairs.size() == 1 && plan.repairs[0].offset == 0x1040, "validated repair");
	uint64_t expected = 0;
	std::memcpy(&expected, bytes.data() + 64, sizeof(expected));
	Require(plan.repairs[0].expected == expected, "repair retains recognized bytes");
	auto changed_after_plan = bytes;
	changed_after_plan[64] ^= 1;
	uint64_t changed = 0;
	std::memcpy(&changed, changed_after_plan.data() + 64, sizeof(changed));
	Require(plan.repairs[0].expected != changed, "stale plan remains distinguishable");
	uint64_t target = 0;
	Require(RelativeTarget(plan.repairs[0].offset + 6, plan.repairs[0].displacement, &target) && target == slots[2], "repair targets declared slot");
	bytes[64] = 0xff; bytes[65] = 0x25;
	std::memcpy(bytes.data() + 66, &plan.repairs[0].displacement, 4);
	Require(find(bytes).matched && find(bytes).repairs.empty(), "idempotent table retains match identity");
	bytes = Fixture(slots); bytes[70] = 0;
	Require(find(bytes).repairs.empty(), "padding mismatch");
	bytes = Fixture(slots); bytes[35] ^= 1;
	Require(find(bytes).repairs.empty(), "anchor mismatch");
	bytes = Fixture(slots); const int32_t local = 0x20;
	std::memcpy(bytes.data() + 65, &local, 4);
	Require(find(bytes).repairs.empty(), "local jump preserved");
	bytes = Fixture(slots); bytes[69] = 0xcc;
	Require(find(bytes).repairs.empty(), "direct encoding mismatch");
	auto duplicate_slots = slots; duplicate_slots[3] = duplicate_slots[2];
	bytes = Fixture(slots);
	Require(Find(bytes.data(), bytes.size(), 0x1000, 0x4000, duplicate_slots).repairs.empty(), "duplicate relocation rejected");
	Require(Find(bytes.data(), 40, 0x1000, 0x4000, slots).repairs.empty(), "truncated table");
	Require(Find(bytes.data(), bytes.size(), UINT64_MAX - 16, UINT64_MAX, slots).repairs.empty(), "segment overflow");
	Require(!RelativeTarget(0, -1, &target), "negative overflow");
	Require(!RelativeTarget(UINT64_MAX, 1, &target), "positive overflow");
	int32_t disp = 0;
	Require(!Displacement(0, UINT64_MAX, &disp), "displacement overflow");
	Require(Displacement(0, INT32_MAX, &disp) && disp == INT32_MAX, "maximum positive displacement");
	Require(Displacement(uint64_t {INT32_MAX} + 1, 0, &disp) && disp == INT32_MIN, "minimum negative displacement");
	Require(RelativeTarget(uint64_t {INT32_MAX} + 1, INT32_MIN, &target) && target == 0, "minimum displacement target");
	Require(!Displacement(0, uint64_t {INT32_MAX} + 1, &disp), "positive displacement just outside range");
	Require(!Displacement(uint64_t {INT32_MAX} + 2, 0, &disp), "negative displacement just outside range");
	// A second fully matching table is ambiguous even when both are plausible.
	std::array<uint8_t, 512> two {};
	auto original = Fixture(slots);
	std::memcpy(two.data(), original.data(), original.size());
	std::memcpy(two.data() + 256, original.data(), original.size());
	for (size_t i = 0; i < slots.size(); ++i)
	{
		if (i == 2) continue;
		const size_t at = 256 + 32 + 16 * i;
		Require(Displacement(0x1000 + at + 6, slots[i], &disp), "second fixture");
		std::memcpy(two.data() + at + 2, &disp, 4);
	}
	plan = Find(two.data(), two.size(), 0x1000, 0x4000, slots);
	Require(plan.ambiguous && plan.repairs.empty(), "ambiguous tables rejected");
	Require(Displacement(0x1046, slots[2], &disp), "pristine first table");
	two[64] = 0xff; two[65] = 0x25;
	std::memcpy(two.data() + 66, &disp, 4);
	plan = Find(two.data(), two.size(), 0x1000, 0x4000, slots);
	Require(plan.ambiguous && plan.repairs.empty(), "pristine and repairable tables remain ambiguous");
	std::puts("PrelinkedPltTest: PASS");
}
