#ifndef INCLUDE_LUMEN_CORE_REFCOUNTER_H_
#define INCLUDE_LUMEN_CORE_REFCOUNTER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/RefCounterPolicy.h"

#include <atomic>
#include <limits>
#include <new>

namespace Lumen::Core {

// Separate owners may copy, release, or detach their shared immutable data.
// Concurrent access to the same owning pointer or mutable payload still needs
// external synchronization. The ownership protocol cannot change while alive.
template <class MutexPolicy>
class RefCounter
{
public:
	RefCounter(): m_atomic(AtomicRefsEnabled())
	{
		if (!m_atomic) new (static_cast<void*>(&m_mutex.value)) MutexPolicy();
	}

	virtual ~RefCounter()
	{
		if (!m_atomic) m_mutex.value.~MutexPolicy();
	}

	void Release() const
	{
		uint32_t previous = 0;
		if (m_atomic)
		{
			previous = m_refs.fetch_sub(1, std::memory_order_acq_rel);
		} else
		{
			m_mutex.value.Lock();
			previous = m_refs.load(std::memory_order_relaxed);
			m_refs.store(previous - 1, std::memory_order_relaxed);
			m_mutex.value.Unlock();
		}
		EXIT_IF(previous == 0);
		if (previous == 1) delete this;
	}

	template <class PtrType>
	void CopyPtr(PtrType** dst, PtrType* src) const
	{
		[[maybe_unused]] uint32_t previous = 0;
		if (m_atomic)
		{
			// The source owner keeps the object alive until the new owner exists.
			previous = m_refs.fetch_add(1, std::memory_order_relaxed);
		} else
		{
			m_mutex.value.Lock();
			previous = m_refs.load(std::memory_order_relaxed);
			m_refs.store(previous + 1, std::memory_order_relaxed);
			m_mutex.value.Unlock();
		}
		EXIT_IF(previous == 0 || previous == (std::numeric_limits<uint32_t>::max)());
		*dst = src;
	}

	template <class PtrType>
	void CopyOnWrite(PtrType** data) const
	{
		uint32_t refs = 0;
		if (m_atomic)
		{
			// Seeing one owner must also observe completion of a former owner's
			// clone before that owner performed its release.
			refs = m_refs.load(std::memory_order_acquire);
		} else
		{
			m_mutex.value.Lock();
			refs = m_refs.load(std::memory_order_relaxed);
			m_mutex.value.Unlock();
		}
		EXIT_IF(refs == 0);
		if (refs > 1)
		{
			// Retain this owner's reference throughout the copy. Releasing it
			// first lets a sibling's Release destroy the source during cloning.
			// A throwing clone leaves both the pointer and ownership unchanged.
			auto* replacement = new PtrType(**data);
			*data = replacement;
			Release();
		}
	}

	LUMEN_CLASS_NO_COPY(RefCounter);

private:
	// Keep the existing storage footprint without constructing a MutexPolicy
	// (Core::Mutex allocates a native mutex) for an atomic ownership object.
	union MutexStorage
	{
		MutexStorage() {}
		~MutexStorage() {}
		MutexPolicy value;
	};

	static_assert(std::atomic<uint32_t>::is_always_lock_free, "RefCounter requires lock-free 32-bit atomics");
	mutable std::atomic<uint32_t> m_refs {1};
	const bool                    m_atomic;
	mutable MutexStorage          m_mutex;
};

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_REFCOUNTER_H_ */
