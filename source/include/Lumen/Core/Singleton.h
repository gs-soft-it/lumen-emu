#ifndef INCLUDE_LUMEN_CORE_SINGLETON_H_
#define INCLUDE_LUMEN_CORE_SINGLETON_H_

#include <cstdlib>
#include <new>

namespace Lumen::Core {

template <class T>
class Singleton
{
public:
	static T* Instance()
	{
		if (!g_m_instance)
		{
			// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
			g_m_instance = static_cast<T*>(std::malloc(sizeof(T)));
			new (g_m_instance) T;
		}

		return g_m_instance;
	}

	LUMEN_CLASS_NO_COPY(Singleton);

protected:
	Singleton();
	~Singleton();

private:
	static inline T* g_m_instance = nullptr;
};

// template<class T> T* Singleton<T>::instance = 0;

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_SINGLETON_H_ */
