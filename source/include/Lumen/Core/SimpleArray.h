#ifndef INCLUDE_LUMEN_CORE_SIMPLEARRAY_H_
#define INCLUDE_LUMEN_CORE_SIMPLEARRAY_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Hash.h"
#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/RefCounter.h"
#include "Lumen/Core/Threads.h"

// IWYU pragma: begin_exports
#include <cstring>
#include <functional>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>
// IWYU pragma: end_exports

// IWYU pragma: private

namespace Lumen::Core {

#define LUMEN_ARRAY_DEFINE_SWAP(name, type)                                                                                                 \
	void name(type* array, int32_t i, int32_t j, [[maybe_unused]] void* arg) /* NOLINT(bugprone-macro-parentheses) */

template <typename T, typename R>
using IsTriviallyCopyable = typename std::enable_if<std::is_trivially_copyable<T>::value, R>::type;

template <typename T, typename R>
using IsNotTriviallyCopyable = typename std::enable_if<!std::is_trivially_copyable<T>::value, R>::type;

template <typename T>
class SimpleArray: public RefCounter<Mutex>
{
public:
	using ValueType       = T;
	using ArrayType       = SimpleArray<T>;
	using SortSwapFunc    = void (*)(T*, int32_t, int32_t, void*);
	using SortCompareFunc = bool (*)(const T&, const T&);

	static constexpr size_t   SIZEOF_T      = sizeof(T); // NOLINT(bugprone-sizeof-expression)
	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);

	SimpleArray() = default;

	SimpleArray(const ArrayType& src) { copy(src); }

	SimpleArray(ArrayType&& src) = delete;

	explicit SimpleArray(uint32_t size, bool ctor = true)
	    : m_values_num(size), m_values_limit(size), m_values(static_cast<T*>(mem_alloc(m_values_limit * SIZEOF_T)))
	{
		if (ctor)
		{
			for (uint32_t index = 0; index < m_values_num; index++)
			{
				new (m_values + index) T();
			}
		}
	}

	SimpleArray(std::initializer_list<T> list): SimpleArray(static_cast<uint32_t>(list.size()))
	{
		T* values_ptr = m_values;
		for (const T& e: list)
		{
			*(values_ptr++) = e;
		}
	}

	~SimpleArray() override { Free(); }

	uint32_t Size() const { return m_values_num; }

	uint32_t Capacity() const { return m_values_limit; }

	uint32_t Hash() { return hash(); } // @suppress("Ambiguous problem")

	void Clear()
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			m_values[index].~T();
		}
		m_values_num = 0;
		m_hash       = 0;
	}

	void Free()
	{
		Clear();
		if (m_values)
		{
			mem_free(m_values);
		}
		m_values_limit = 0;
		m_values       = nullptr;
	}

	// NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp)
	SimpleArray<T>& operator=(const ArrayType& src)
	{
		if (this != &src)
		{
			Free();
			copy(src);
		}
		return *this;
	}

	ArrayType& operator=(ArrayType&& src) = delete;

	bool operator==(const ArrayType& s) const
	{
		if (m_values_num != s.m_values_num)
		{
			return false;
		}

		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (!(m_values[index] == s.m_values[index]))
			{
				return false;
			}
		}
		return true;
	}

	void Expand(uint32_t num) { expand(num); } // @suppress("Ambiguous problem")

	void Add(const T& val)
	{
		uint32_t values_i = m_values_num;
		expand(1); // @suppress("Ambiguous problem")
		m_values_num = values_i + 1;
		new (m_values + values_i) T(val);
		m_hash = 0;
	}

	void Add(T&& val)
	{
		uint32_t values_i = m_values_num;
		expand(1); // @suppress("Ambiguous problem")
		m_values_num = values_i + 1;
		new (m_values + values_i) T(std::forward<T>(val));
		m_hash = 0;
	}

	void Add(const T* val, uint32_t num)
	{
		uint32_t values_i = m_values_num;
		expand(num); // @suppress("Ambiguous problem")
		m_values_num = values_i + num;
		for (uint32_t i = 0; i < num; i++)
		{
			new (m_values + values_i + i) T(val[i]);
		}
		m_hash = 0;
	}

	void InsertAt(uint32_t index, const T& val)
	{
		Add(val);
		if (IndexValid(index))
		{
			for (uint32_t last_index = m_values_num - 1; last_index != index; last_index--)
			{
				std::swap(m_values[last_index], m_values[last_index - 1]);
			}
		}
	}

	void InsertAt(uint32_t index, T&& val)
	{
		Add(std::forward<T>(val));
		if (IndexValid(index))
		{
			for (uint32_t last_index = m_values_num - 1; last_index != index; last_index--)
			{
				std::swap(m_values[last_index], m_values[last_index - 1]);
			}
		}
	}

	uint32_t Find(const T& t) const
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (m_values[index] == t)
			{
				return index;
			}
		}
		return INVALID_INDEX;
	}

	template <typename OP>
	uint32_t Find(const T& t, OP&& op_eq) const
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (op_eq(m_values[index], t))
			{
				return index;
			}
		}
		return INVALID_INDEX;
	}

	template <typename T2, typename OP>
	uint32_t Find(const T2& t, OP&& op_eq) const
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (op_eq(m_values[index], t))
			{
				return index;
			}
		}
		return INVALID_INDEX;
	}

	template <typename T2, typename OP, typename V>
	void FindAll(const T2& t, OP&& op_eq, V* ret) const
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (op_eq(m_values[index], t))
			{
				ret->Add(index);
			}
		}
	}

	template <typename T2, typename T3, typename OP>
	uint32_t Find(const T2& t2, const T3& t3, OP&& op_eq) const
	{
		for (uint32_t index = 0; index < m_values_num; index++)
		{
			if (op_eq(m_values[index], t2, t3))
			{
				return index;
			}
		}
		return INVALID_INDEX;
	}

	bool Remove(const T& t)
	{
		uint32_t index = Find(t);
		if (index == INVALID_INDEX)
		{
			return false;
		}
		return RemoveAt(index);
	}

	bool RemoveAt(uint32_t index, uint32_t count = 1) { return remove_at(index, count); } // @suppress("Ambiguous problem")

	[[nodiscard]] bool IndexValid(uint32_t index) const { return index < m_values_num; }

	T& operator[](uint32_t index)
	{
		m_hash = 0;

		if (index >= m_values_num)
		{
			EXIT_IF(index >= m_values_num);
		}

		return m_values[index];
	}

	const T& operator[](uint32_t index) const
	{
		EXIT_IF(index >= m_values_num);

		return m_values[index];
	}

	const T& At(uint32_t index) const
	{
		if (index >= m_values_num)
		{
			EXIT_IF(index >= m_values_num);
		}

		return m_values[index];
	}

	T* GetData()
	{
		m_hash = 0;
		return m_values;
	}

	const T* GetData() const { return m_values; }

	const T* GetDataConst() const { return m_values; }

	void Memset(int c)
	{
		m_hash = 0;
		memset(m_values, c, m_values_num * SIZEOF_T);
	}

	void Sort()
	{
		if (m_values_num == 0)
		{
			return;
		}

		sort(0, m_values_num - 1, sort_depth_limit(m_values_num));
		m_hash = 0;
	}

	void Sort(SortCompareFunc comp_func)
	{
		if (m_values_num == 0)
		{
			return;
		}

		sort_with_compare_func(0, m_values_num - 1, sort_depth_limit(m_values_num), comp_func);
		m_hash = 0;
	}

	void Sort(SortSwapFunc swap_func, void* swap_arg)
	{
		if (m_values_num == 0)
		{
			return;
		}

		sort_with_swap_func(0, m_values_num - 1, sort_depth_limit(m_values_num), swap_func, swap_arg);
		m_hash = 0;
	}

	template <typename OP>
	void Sort(OP&& comp_func)
	{
		if (m_values_num == 0)
		{
			return;
		}

		sort_with_compare_func(0, m_values_num - 1, sort_depth_limit(m_values_num), comp_func);
		m_hash = 0;
	}

	using iterator       = T*;
	using const_iterator = const T*;

	iterator begin() // NOLINT(readability-identifier-naming)
	{
		m_hash = 0;
		return m_values;
	}
	iterator end() // NOLINT(readability-identifier-naming)
	{
		m_hash = 0;
		return m_values + m_values_num;
	}
	const_iterator begin() const { return m_values; }               // NOLINT(readability-identifier-naming)
	const_iterator end() const { return m_values + m_values_num; }  // NOLINT(readability-identifier-naming)
	const_iterator cbegin() const { return m_values; }              // NOLINT(readability-identifier-naming)
	const_iterator cend() const { return m_values + m_values_num; } // NOLINT(readability-identifier-naming)

private:
	template <typename U = T>
	IsNotTriviallyCopyable<U, void> copy(const ArrayType& src)
	{
		m_values_num   = src.m_values_num;
		m_values_limit = src.m_values_limit;
		m_hash         = src.m_hash;
		if (src.m_values)
		{
			m_values = static_cast<T*>(mem_alloc(m_values_limit * SIZEOF_T));
			for (uint32_t index = 0; index < m_values_num; index++)
			{
				new (m_values + index) T(src.m_values[index]);
			}
		} else
		{
			m_values = nullptr;
		}
	}

	template <typename U = T>
	IsTriviallyCopyable<U, void> copy(const ArrayType& src)
	{
		m_values_num   = src.m_values_num;
		m_values_limit = src.m_values_limit;
		m_hash         = src.m_hash;
		if (src.m_values)
		{
			m_values = static_cast<T*>(mem_alloc(m_values_limit * SIZEOF_T));
			std::memcpy(m_values, src.m_values, m_values_num * SIZEOF_T);
		} else
		{
			m_values = nullptr;
		}
	}

	template <typename U = T>
	IsNotTriviallyCopyable<U, void> expand(uint32_t add_num)
	{
		uint32_t values_i       = m_values_num;
		uint32_t new_values_num = values_i + add_num;
		if (new_values_num >= m_values_limit)
		{
			m_values_limit = static_cast<uint32_t>((m_values_limit * 3) / 2 + new_values_num - m_values_limit + 1);
			T* old_values  = m_values;
			m_values       = static_cast<T*>(mem_alloc(m_values_limit * SIZEOF_T));
			for (uint32_t index = 0; index < values_i; index++)
			{
				new (m_values + index) T(old_values[index]);
				old_values[index].~T();
			}
			mem_free(old_values);
		}
	}

	template <typename U = T>
	IsTriviallyCopyable<U, void> expand(uint32_t add_num)
	{
		uint32_t values_i       = m_values_num;
		uint32_t new_values_num = values_i + add_num;
		if (new_values_num >= m_values_limit)
		{
			m_values_limit = static_cast<uint32_t>((m_values_limit * 3) / 2 + new_values_num - m_values_limit + 1);
			m_values       = static_cast<T*>(mem_realloc(m_values, m_values_limit * SIZEOF_T));
		}
	}

	static uint32_t sort_depth_limit(uint32_t count)
	{
		uint32_t limit = 0;
		while (count > 1)
		{
			count >>= 1u;
			limit += 2;
		}
		return limit;
	}

	void sort_swap(uint32_t left, uint32_t right)
	{
		// Preserve support for copy-only values (including deleted move members).
		const T temporary = m_values[left];
		m_values[left] = m_values[right];
		m_values[right] = temporary;
	}

	template <typename OP, typename SWAP>
	void heap_sort(int32_t low, int32_t high, OP&& comp_func, SWAP&& swap_func)
	{
		// An adversarial middle pivot can exhaust quicksort's depth budget even
		// for a valid strict ordering. Finish in O(n log n), without recursion or
		// allocation. All permutations go through the supplied swap operation.
		const auto base = static_cast<uint32_t>(low);
		const auto count = static_cast<uint32_t>(high) - base + 1;
		const auto sift_down = [&](uint32_t root, uint32_t size) {
			while (root < size / 2)
			{
				uint32_t child = root * 2 + 1;
				if (child + 1 < size && comp_func(m_values[base + child], m_values[base + child + 1])) ++child;
				if (!comp_func(m_values[base + root], m_values[base + child])) break;
				swap_func(base + root, base + child);
				root = child;
			}
		};
		for (uint32_t start = count / 2; start != 0; --start) sift_down(start - 1, count);
		for (uint32_t end = count; end > 1; --end)
		{
			swap_func(base, base + end - 1);
			sift_down(0, end - 1);
		}
	}

	void sort(int32_t low, int32_t high, uint32_t depth_remaining)
	{
		EXIT_IF(low < 0 || high < 0);
		if (depth_remaining == 0)
		{
			heap_sort(low, high, [](const T& left, const T& right) { return left < right; },
			          [this](uint32_t left, uint32_t right) { sort_swap(left, right); });
			return;
		}

		int32_t i = low;
		int32_t j = high;

		const T m = m_values[static_cast<uint32_t>(low + (high - low) / 2)];
		do
		{
			while (m_values[i] < m)
			{
				i++;
			}
			while (m < m_values[j])
			{
				j--;
			}
			if (i <= j)
			{
				sort_swap(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
				i++;
				j--;
			}
		} while (i <= j);

		if (low < j)
		{
			sort(low, j, depth_remaining - 1);
		}
		if (i < high)
		{
			sort(i, high, depth_remaining - 1);
		}
	}

	template <typename OP>
	void sort_with_compare_func(int32_t low, int32_t high, uint32_t depth_remaining, OP&& comp_func)
	{
		EXIT_IF(low < 0 || high < 0);
		if (depth_remaining == 0)
		{
			heap_sort(low, high, comp_func,
			          [this](uint32_t left, uint32_t right) { sort_swap(left, right); });
			return;
		}

		int32_t i = low;
		int32_t j = high;

		const T m = m_values[static_cast<uint32_t>(low + (high - low) / 2)];
		do
		{
			while (comp_func(m_values[i], m))
			{
				i++;
			}
			while (comp_func(m, m_values[j]))
			{
				j--;
			}
			if (i <= j)
			{
				sort_swap(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
				i++;
				j--;
			}
		} while (i <= j);

		if (low < j)
		{
			sort_with_compare_func(low, j, depth_remaining - 1, comp_func);
		}
		if (i < high)
		{
			sort_with_compare_func(i, high, depth_remaining - 1, comp_func);
		}
	}

	void sort_with_swap_func(int32_t low, int32_t high, uint32_t depth_remaining, SortSwapFunc swap_func, void* arg)
	{
		EXIT_IF(low < 0 || high < 0);
		if (depth_remaining == 0)
		{
			heap_sort(low, high, [](const T& left, const T& right) { return left < right; },
			          [this, swap_func, arg](uint32_t left, uint32_t right) {
				          swap_func(m_values, static_cast<int32_t>(left), static_cast<int32_t>(right), arg);
			          });
			return;
		}

		int32_t i = low;
		int32_t j = high;

		const T m = m_values[static_cast<uint32_t>(low + (high - low) / 2)];
		do
		{
			while (m_values[i] < m)
			{
				i++;
			}
			while (m < m_values[j])
			{
				j--;
			}
			if (i <= j)
			{
				swap_func(m_values, i, j, arg);
				i++;
				j--;
			}
		} while (i <= j);

		if (low < j)
		{
			sort_with_swap_func(low, j, depth_remaining - 1, swap_func, arg);
		}
		if (i < high)
		{
			sort_with_swap_func(i, high, depth_remaining - 1, swap_func, arg);
		}
	}

	template <typename U = T>
	IsNotTriviallyCopyable<U, uint32_t> hash()
	{
		if (m_hash == 0)
		{
			EXIT_IF(true);
		}

		return m_hash;
	}

	template <typename U = T>
	IsTriviallyCopyable<U, uint32_t> hash()
	{
		if (m_hash == 0)
		{
			m_hash = Core::hash(m_values, SIZEOF_T * m_values_num);
		}

		return m_hash;
	}

	template <typename U = T>
	IsNotTriviallyCopyable<U, bool> remove_at(uint32_t index, uint32_t count = 1)
	{
		uint32_t size = m_values_num;
		if (index >= size)
		{
			return false;
		}

		if (index + count > size)
		{
			count = size - index;
		}

		m_values_num = size - count;

		uint32_t mc = size - (index + count);
		for (uint32_t i = 0; i < mc; i++)
		{
			m_values[index + i] = m_values[index + i + count];
		}

		for (uint32_t i = 0; i < count; i++)
		{
			m_values[m_values_num + i].~T();
		}

		m_hash = 0;

		return true;
	}

	template <typename U = T>
	IsTriviallyCopyable<U, bool> remove_at(uint32_t index, uint32_t count = 1)
	{
		uint32_t size = m_values_num;
		if (index >= size)
		{
			return false;
		}

		if (index + count > size)
		{
			count = size - index;
		}

		m_values_num = size - count;

		std::memmove(m_values + index, (m_values + index + count), (size - (index + count)) * SIZEOF_T);

		for (uint32_t i = 0; i < count; i++)
		{
			m_values[m_values_num + i].~T();
		}

		m_hash = 0;

		return true;
	}

	uint32_t m_values_num {0};
	uint32_t m_values_limit {0};
	T*       m_values {nullptr};
	uint32_t m_hash {0};
};

#define FOR(index, array) /* NOLINT(cppcoreguidelines-macro-usage)*/                                                                       \
	uint32_t array##_size_ = (array).Size();                                                                                               \
	for (uint32_t index = 0; index < array##_size_; index++) /* NOLINT(bugprone-macro-parentheses) */

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_SIMPLEARRAY_H_ */
