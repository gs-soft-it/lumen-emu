#ifndef INCLUDE_LUMEN_CORE_SUBSYSTEMS_H_
#define INCLUDE_LUMEN_CORE_SUBSYSTEMS_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Singleton.h"

#include <initializer_list>

namespace Lumen::Core {

class Subsystem;
class SubsystemsListPrivate;
class SubsystemPrivate;

class SubsystemsList
{
public:
	SubsystemsList();
	virtual ~SubsystemsList();
	void SetArgs(int argc, char* argv[]);

	// void Add(Subsystem* s, const char* name, ...);
	void Add(Subsystem* s, std::initializer_list<Subsystem*> deps);

	bool InitAll(bool print_msg = false);
	void DestroyAll(bool print_msg = false);

	// Release the registration graph after every initialized subsystem has
	// already been destroyed. This does not make incomplete subsystem Destroy
	// implementations safe to initialize again; callers must provide a real
	// teardown before using the list for another lifecycle.
	[[nodiscard]] bool ResetRegistrations();

	int*   GetArgc();
	char** GetArgv();

	[[nodiscard]] const char* GetFailName() const;
	[[nodiscard]] const char* GetFailMsg() const;

	void ShutdownAll();

	static SubsystemsList* Instance() { return Core::Singleton<SubsystemsList>::Instance(); }

	LUMEN_CLASS_NO_COPY(SubsystemsList);

private:
	SubsystemsListPrivate* m_p;
};

using SubsystemsListSingleton = Lumen::Core::Singleton<SubsystemsList>;

class Subsystem
{
public:
	Subsystem();
	virtual ~Subsystem();

	virtual const char* Id()                                       = 0;
	virtual void        Init(SubsystemsList* parent)               = 0;
	virtual void        Destroy(SubsystemsList* parent)            = 0;
	virtual void        UnexpectedShutdown(SubsystemsList* parent) = 0;

	friend class SubsystemsListPrivate;

	LUMEN_CLASS_NO_COPY(Subsystem);

protected:
	void Fail(const char* format, ...) LUMEN_FORMAT_PRINTF(2, 3);

private:
	SubsystemPrivate* m_p;
};

#define LUMEN_SUBSYSTEM_DEFINE(s)                                                                                                           \
	class s##Subsystem: public Core::Subsystem                                                                                             \
	{                                                                                                                                      \
	public:                                                                                                                                \
		static Subsystem* Instance() { return Core::Singleton<s##Subsystem>::Instance(); }                                                 \
		const char*       Id() { return #s; }                                                                                              \
		void              Init(Core::SubsystemsList* parent);                                                                              \
		void              Destroy(Core::SubsystemsList* parent);                                                                           \
		void              UnexpectedShutdown(Core::SubsystemsList* parent);                                                                \
	};                                                                                                                                     \
	typedef Core::Singleton<s##Subsystem> s##SubsystemSingleton;

#define LUMEN_SUBSYSTEM_INIT(s)                void s##Subsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
#define LUMEN_SUBSYSTEM_DESTROY(s)             void s##Subsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
#define LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(s) void s##Subsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)

#define LUMEN_SUBSYSTEM_ARGC parent->GetArgc()
#define LUMEN_SUBSYSTEM_ARGV parent->GetArgv()

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#define LUMEN_SUBSYSTEM_FAIL(f, ...)                                                                                                        \
	this->Fail(f, __VA_ARGS__);                                                                                                            \
	return;
#else
#define LUMEN_SUBSYSTEM_FAIL(f, s...)                                                                                                       \
	this->Fail(f, ##s);                                                                                                                    \
	return;
#endif

//#define LUMEN_SUBSYSTEM_ADD(list, s, ...) list.Add(s##SubsystemSingleton::Instance(), #s, __VA_ARGS__);
//#define LUMEN_SUBSYSTEM_ADD2(list, ...)   list.Add2(s##SubsystemSingleton::Instance(), __VA_ARGS__);

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_SUBSYSTEMS_H_ */
