#ifndef INCLUDE_LUMEN_CORE_SAFEDELETE_H_
#define INCLUDE_LUMEN_CORE_SAFEDELETE_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"

#include <cstring>

namespace Lumen {

#define LUMEN_CORE_SAFE_DELETE_DEADBEEF (reinterpret_cast<void*>((uintptr_t)0x01))

#define DeleteProtected(p) /* NOLINT(cppcoreguidelines-macro-usage) */                                                                     \
	{                                                                                                                                      \
		if (!(p))                                                                                                                          \
		{                                                                                                                                  \
			EXIT("delete null\n");                                                                                                         \
		}                                                                                                                                  \
		void* dddd = LUMEN_CORE_SAFE_DELETE_DEADBEEF;                                                                                       \
		if (std::memcmp(&(p), &dddd, sizeof(void*)) == 0)                                                                                  \
		{                                                                                                                                  \
			EXIT("already deleted\n");                                                                                                     \
		}                                                                                                                                  \
		delete (p);                                                                                                                        \
		std::memcpy(&(p), &dddd, sizeof(void*));                                                                                           \
	}

#define DeleteProtectedArray(p) /* NOLINT(cppcoreguidelines-macro-usage) */                                                                \
	{                                                                                                                                      \
		if (!(p))                                                                                                                          \
		{                                                                                                                                  \
			EXIT("delete null\n");                                                                                                         \
		}                                                                                                                                  \
		void* dddd = LUMEN_CORE_SAFE_DELETE_DEADBEEF;                                                                                       \
		if (std::memcmp(&(p), &dddd, sizeof(void*)) == 0)                                                                                  \
		{                                                                                                                                  \
			EXIT("already deleted\n");                                                                                                     \
		}                                                                                                                                  \
		delete[](p);                                                                                                                       \
		std::memcpy(&(p), &dddd, sizeof(void*));                                                                                           \
	}

template <class T>
void Delete(T*& p)
{
	if (!p)
	{
		EXIT("delete null\n");
	}
	if (p == static_cast<T*>(LUMEN_CORE_SAFE_DELETE_DEADBEEF))
	{
		EXIT("already deleted\n");
	}
	delete p;
	p = static_cast<T*>(LUMEN_CORE_SAFE_DELETE_DEADBEEF);
}

template <class T>
void DeleteArray(T*& p)
{
	if (!p)
	{
		EXIT("delete null\n");
	}
	if (p == static_cast<T*>(LUMEN_CORE_SAFE_DELETE_DEADBEEF))
	{
		EXIT("already deleted\n");
	}
	delete[] p;
	p = static_cast<T*>(LUMEN_CORE_SAFE_DELETE_DEADBEEF);
}

} // namespace Lumen

#endif /* INCLUDE_LUMEN_CORE_SAFEDELETE_H_ */
