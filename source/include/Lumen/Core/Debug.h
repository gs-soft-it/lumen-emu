#ifndef INCLUDE_LUMEN_CORE_DEBUG_H_
#define INCLUDE_LUMEN_CORE_DEBUG_H_

#include "Lumen/Core/ArrayWrapper.h"
#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

namespace Lumen::Core {

void core_debug_init(const char* app_name);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_BUILD == LUMEN_BUILD_DEBUG && LUMEN_COMPILER == LUMEN_COMPILER_CLANG
constexpr int DEBUG_MAX_STACK_DEPTH = 20;
#else
constexpr int DEBUG_MAX_STACK_DEPTH = 15;
#endif

struct DebugMapPrivate;

namespace Debug {
String GetCompiler();
String GetLinker();
String GetBitness();
} // namespace Debug

class DebugMap
{
public:
	DebugMap();
	virtual ~DebugMap();

	void LoadMap();
	void LoadCsv();

	void LoadMsvcLink(const String& name, int mode);
	void LoadMsvcLldLink(const String& name, int mode);
	void LoadGnuLd(const String& name, int bitness);
	void LoadLlvmLld(const String& name, int bitness);
	void LoadCsv(const String& name);

	void DumpMap(const String& name);

	friend struct DebugMapPrivate;

	LUMEN_CLASS_NO_COPY(DebugMap);

private:
	DebugMapPrivate* m_p;
};

struct DebugStack
{
	int                                 depth {0};
	Array<void*, DEBUG_MAX_STACK_DEPTH> stack {};

	[[nodiscard]] uintptr_t GetAddr(int i) const { return reinterpret_cast<uintptr_t>(stack[i]); }

	void Print(int from, bool with_name = true) const;
	void PrintAndroid(int from, bool with_name = true) const;

	void CopyTo(DebugStack* s) const
	{
		std::memcpy(s->stack, stack, sizeof(void*) * depth);
		s->depth = depth;
	}

	static void Trace(DebugStack* stack);
};

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_DEBUG_H_ */
