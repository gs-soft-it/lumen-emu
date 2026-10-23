#include "Lumen/Core/SimpleArray.h"
#include "Lumen/Core/Vector.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <type_traits>
#include <vector>

namespace {

using Lumen::Core::SimpleArray;
using Lumen::Vector;
uint64_t comparisons = 0;

bool Check(bool valid, const char* message)
{
	if (!valid) std::fprintf(stderr, "SimpleArraySortTest: %s\n", message);
	return valid;
}

// Place the next minimum at the middle of each suffix after its previous
// partition swap. These are fixed distinct integers, compared only with <.
// The former quicksort reaches depth 64 on just 66 entries (low 64/high 65).
std::vector<uint32_t> MiddleMinimum(uint32_t count)
{
	std::vector<uint32_t> positions(count), values(count);
	std::iota(positions.begin(), positions.end(), 0u);
	for (uint32_t low = 0; low + 1 < count; ++low)
	{
		const uint32_t middle = low + (count - 1 - low) / 2;
		values[positions[middle]] = low;
		std::swap(positions[low], positions[middle]);
	}
	if (count != 0) values[positions.back()] = count - 1;
	return values;
}

bool Less(const uint32_t& left, const uint32_t& right)
{
	++comparisons;
	return left < right;
}

bool Greater(const uint32_t& left, const uint32_t& right)
{
	++comparisons;
	return left > right;
}

uint64_t ComparisonLimit(uint32_t count)
{
	uint64_t levels = 0;
	for (uint32_t remaining = count; remaining > 1; remaining >>= 1u) ++levels;
	// Generous O(n log n) ceiling. An unbounded middle-pivot quicksort needs
	// over 8 million comparisons on the 4096-entry fixture and fails this bound.
	return 16 * static_cast<uint64_t>(count) * (levels + 1);
}

bool Numbers(const std::vector<uint32_t>& input, unsigned mode)
{
	SimpleArray<uint32_t> array;
	for (auto value: input) array.Add(value);
	const auto prior_hash = array.Hash();
	SimpleArray<uint32_t> preserved(array); // Includes the cached hash.
	std::vector<uint32_t> expected = input;
	const bool descending = mode == 2;
	std::sort(expected.begin(), expected.end(), [descending](uint32_t left, uint32_t right) {
		return descending ? left > right : left < right;
	});
	comparisons = 0;
	if (mode == 0) array.Sort();
	else if (mode == 1) array.Sort(&Less);
	else if (mode == 2) array.Sort(&Greater);
	else array.Sort([&](const uint32_t& left, const uint32_t& right) { ++comparisons; return left < right; });
	const auto count = static_cast<uint32_t>(input.size());
	bool valid = array.Size() == count && preserved.Size() == count && comparisons <= ComparisonLimit(count);
	for (uint32_t index = 0; index < count; ++index)
		valid = valid && array.At(index) == expected[index] && preserved.At(index) == input[index];
	const auto expected_hash = Lumen::Core::hash(expected.data(), count * sizeof(uint32_t));
	valid = valid && array.Hash() == expected_hash && preserved.Hash() == prior_hash;
	// On the adversarial fixture these differ, so the equality above actually
	// verifies that sorting invalidated the copied/cached input hash.
	if (input == MiddleMinimum(66)) valid = valid && prior_hash != expected_hash;
	return Check(valid, "numeric ordering, comparison bound, copy or hash invalidation");
}

struct CopyOnly
{
	uint32_t value = 0;
	CopyOnly() = default;
	CopyOnly(const CopyOnly&) = default;
	CopyOnly& operator=(const CopyOnly&) = default;
	CopyOnly(CopyOnly&&) = delete;
	CopyOnly& operator=(CopyOnly&&) = delete;
	~CopyOnly() {} // Exercise nontrivial storage/copy paths too.
	bool operator<(const CopyOnly& other) const { ++comparisons; return value < other.value; }
};

bool CopyOnlyValues()
{
	static_assert(!std::is_move_constructible_v<CopyOnly> && !std::is_trivially_copyable_v<CopyOnly>);
	const auto input = MiddleMinimum(4096);
	SimpleArray<CopyOnly> array(static_cast<uint32_t>(input.size()));
	for (uint32_t index = 0; index < array.Size(); ++index) array[index].value = input[index];
	comparisons = 0;
	array.Sort();
	bool valid = comparisons <= ComparisonLimit(array.Size());
	for (uint32_t index = 0; index < array.Size(); ++index) valid = valid && array.At(index).value == index;
	for (uint32_t index = 0; index < array.Size(); ++index) array[index].value = input[index];
	comparisons = 0;
	array.Sort([](const CopyOnly& left, const CopyOnly& right) { ++comparisons; return left.value < right.value; });
	valid = valid && comparisons <= ComparisonLimit(array.Size());
	for (uint32_t index = 0; index < array.Size(); ++index) valid = valid && array.At(index).value == index;
	return Check(valid, "copy-only value sort or comparison bound");
}

struct IndexedValue
{
	uint32_t key = 0;
	uint32_t identity = 0;
	bool operator<(const IndexedValue& other) const { ++comparisons; return key < other.key; }
};

struct SwapContext
{
	std::vector<uint32_t> positions;
	uint32_t calls = 0;
	bool valid = true;
};

void SwapAndTrack(IndexedValue* array, int32_t left, int32_t right, void* opaque)
{
	auto& context = *static_cast<SwapContext*>(opaque);
	++context.calls;
	if (left < 0 || right < 0 || static_cast<uint32_t>(left) >= context.positions.size() ||
	    static_cast<uint32_t>(right) >= context.positions.size())
	{
		context.valid = false;
		return;
	}
	context.valid = context.valid && context.positions[array[left].identity] == static_cast<uint32_t>(left) &&
	                context.positions[array[right].identity] == static_cast<uint32_t>(right);
	// Match callers which update their external index before performing the swap.
	context.positions[array[left].identity] = static_cast<uint32_t>(right);
	context.positions[array[right].identity] = static_cast<uint32_t>(left);
	const IndexedValue temporary = array[left];
	array[left] = array[right];
	array[right] = temporary;
	for (uint32_t index = 0; index < context.positions.size(); ++index)
		context.valid = context.valid && context.positions[array[index].identity] == index;
}

bool CustomSwap(bool duplicates)
{
	const auto input = MiddleMinimum(128);
	SimpleArray<IndexedValue> array(static_cast<uint32_t>(input.size()));
	SwapContext context;
	context.positions.resize(input.size());
	std::iota(context.positions.begin(), context.positions.end(), 0u);
	for (uint32_t index = 0; index < array.Size(); ++index)
		array[index] = {duplicates ? input[index] % 7 : input[index], index};
	(void)array.Hash();
	comparisons = 0;
	array.Sort(SwapAndTrack, &context);
	bool valid = context.valid && context.calls != 0 && comparisons <= ComparisonLimit(array.Size());
	for (uint32_t index = 0; index < array.Size(); ++index)
		valid = valid && context.positions[array.At(index).identity] == index &&
		        (index == 0 || array.At(index - 1).key <= array.At(index).key);
	valid = valid && array.Hash() == Lumen::Core::hash(array.GetDataConst(), array.Size() * sizeof(IndexedValue));
	return Check(valid, "custom swap bypassed, index invalidated, order or hash wrong");
}

bool CopyOnWrite()
{
	const auto input = MiddleMinimum(128);
	Vector<uint32_t> original;
	for (auto value: input) original.Add(value);
	Vector<uint32_t> sorted = original;
	sorted.Sort(&Less);
	for (uint32_t index = 0; index < original.Size(); ++index)
		if (!Check(original.At(index) == input[index] && sorted.At(index) == index, "Vector copy-on-write sort")) return false;
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::core_memory_init();
	if (argc == 2 && std::strcmp(argv[1], "--regression-probe") == 0)
	{
		std::puts("SimpleArraySortTest: regression-probe 66 strict-less");
		std::fflush(stdout);
		if (!Numbers(MiddleMinimum(66), 1)) return 1;
		std::puts("SimpleArraySortTest: PASS");
		return 0;
	}
	if (argc != 1) return 2;
	std::vector<std::vector<uint32_t>> cases {{}, {7}, {9, 1}, MiddleMinimum(66), MiddleMinimum(4096)};
	cases.emplace_back(1024, 5);
	std::vector<uint32_t> descending(1024), repeated(1024);
	for (uint32_t index = 0; index < descending.size(); ++index)
	{
		descending[index] = static_cast<uint32_t>(descending.size() - index);
		repeated[index] = (index * 37) % 11;
	}
	cases.push_back(descending);
	cases.push_back(repeated);
	// Mirror the killer so descending comparison also exhausts the budget.
	auto reversed = MiddleMinimum(4096);
	for (auto& value: reversed) value = static_cast<uint32_t>(reversed.size() - 1) - value;
	cases.push_back(reversed);
	for (const auto& input: cases)
		for (unsigned mode = 0; mode != 4; ++mode)
			if (!Numbers(input, mode)) return 1;
	if (!CopyOnlyValues() || !CustomSwap(false) || !CustomSwap(true) || !CopyOnWrite()) return 1;
	std::puts("SimpleArraySortTest: PASS");
	return 0;
}
