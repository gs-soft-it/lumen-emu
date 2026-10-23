#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

namespace Trophy = Lumen::Libs::Network::NpTrophy;
namespace Network = Lumen::Libs::Network;
namespace Posix = Lumen::Libs::Posix;
namespace Vm = Lumen::Core::VirtualMemory;

struct GameDetailsMirror
{
	uint64_t size;
	uint32_t groups;
	uint32_t trophies;
	uint32_t platinum;
	uint32_t gold;
	uint32_t silver;
	uint32_t bronze;
	uint8_t title[128];
	uint8_t description[1024];
};

struct GameDataMirror
{
	uint64_t size;
	uint32_t unlocked;
	uint32_t platinum;
	uint32_t gold;
	uint32_t silver;
	uint32_t bronze;
	uint32_t progress;
};

static_assert(sizeof(GameDetailsMirror) == 1184);
static_assert(offsetof(GameDetailsMirror, size) == 0);
static_assert(offsetof(GameDetailsMirror, groups) == 8);
static_assert(offsetof(GameDetailsMirror, trophies) == 12);
static_assert(offsetof(GameDetailsMirror, platinum) == 16);
static_assert(offsetof(GameDetailsMirror, gold) == 20);
static_assert(offsetof(GameDetailsMirror, silver) == 24);
static_assert(offsetof(GameDetailsMirror, bronze) == 28);
static_assert(offsetof(GameDetailsMirror, title) == 32);
static_assert(offsetof(GameDetailsMirror, description) == 160);
static_assert(sizeof(GameDataMirror) == 32);
static_assert(offsetof(GameDataMirror, size) == 0);
static_assert(offsetof(GameDataMirror, unlocked) == 8);
static_assert(offsetof(GameDataMirror, platinum) == 12);
static_assert(offsetof(GameDataMirror, gold) == 16);
static_assert(offsetof(GameDataMirror, silver) == 20);
static_assert(offsetof(GameDataMirror, bronze) == 24);
static_assert(offsetof(GameDataMirror, progress) == 28);

constexpr size_t DETAILS_BYTES = sizeof(GameDetailsMirror);
constexpr size_t DATA_BYTES = sizeof(GameDataMirror);
constexpr size_t VM_UNIT = 65536;
constexpr int ERRNO_SENTINEL = 21731;
constexpr size_t WORK_BYTES = DETAILS_BYTES * 2 + 128;
static_assert(sizeof(int) == sizeof(int32_t));

using GameInfoFunction = decltype(&Trophy::NpTrophyGetGameInfo);
GameInfoFunction g_get_game_info = &Trophy::NpTrophyGetGameInfo;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpTrophyBindingTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name = nid;
	query.library = "libSceNpTrophy";
	query.library_version = 1;
	query.module = "libSceNpTrophy";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	return query;
}

bool Missing(const Lumen::Loader::SymbolDatabase& symbols, const Lumen::Loader::SymbolResolve& query,
             const char* nid, const char* mismatch)
{
	if (symbols.Find(query) == nullptr) return true;
	std::fprintf(stderr, "NpTrophyBindingTest: %s resolved with unsupported %s\n", nid, mismatch);
	return false;
}

bool NegativeIdentity(const Lumen::Loader::SymbolDatabase& symbols, const char* nid)
{
	const auto exact = Exact(nid);
	bool passed = true;
	for (int version : {0, 2})
	{
		auto wrong = exact;
		wrong.library_version = version;
		passed = Missing(symbols, wrong, nid, "library version") && passed;
		wrong = exact;
		wrong.module_version_major = version;
		passed = Missing(symbols, wrong, nid, "module major version") && passed;
		wrong = exact;
		wrong.module_version_minor = version;
		passed = Missing(symbols, wrong, nid, "module minor version") && passed;
	}
	for (const char* other : {"libSceNpManager", "libSceNpCommon", "libSceNpManagerForToolkit",
	                         "libSceNet", "unrelated_trophy_library"})
	{
		auto wrong = exact;
		wrong.library = other;
		passed = Missing(symbols, wrong, nid, "library namespace") && passed;
		wrong = exact;
		wrong.module = other;
		passed = Missing(symbols, wrong, nid, "module namespace") && passed;
	}
	for (auto type : {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
	                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
	{
		auto wrong = exact;
		wrong.type = type;
		passed = Missing(symbols, wrong, nid, "symbol type") && passed;
	}
	return passed;
}

bool TestBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libNet_1", &symbols), "libNet_1 registration failed")) return false;
	struct Existing { const char* nid; uint64_t address; };
	const std::array<Existing, 5> existing = {{
	    {"q7U6tEAQf7c", reinterpret_cast<uint64_t>(&Trophy::NpTrophyCreateHandle)},
	    {"XbkjbobZlCY", reinterpret_cast<uint64_t>(&Trophy::NpTrophyCreateContext)},
	    {"TJCAxto9SEU", reinterpret_cast<uint64_t>(&Trophy::NpTrophyRegisterContext)},
	    {"GNcF4oidY0Y", reinterpret_cast<uint64_t>(&Trophy::NpTrophyDestroyHandle)},
	    {"LHuSmO3SLd8", reinterpret_cast<uint64_t>(&Trophy::NpTrophyGetTrophyUnlockState)}}};
	bool passed = true;
	for (const auto& binding : existing)
	{
		const auto* symbol = symbols.Find(Exact(binding.nid));
		if (symbol == nullptr || symbol->vaddr != binding.address)
		{
			std::fprintf(stderr, "NpTrophyBindingTest: existing binding changed: %s\n", binding.nid);
			passed = false;
		}
		passed = NegativeIdentity(symbols, binding.nid) && passed;
	}

	// The approved local model qualifies this encoded identity only in the exact
	// Trophy namespace and deliberately provides no standard-name fallback.
	constexpr const char* observed = "YYP3f2W09og";
	const auto* candidate = symbols.Find(Exact(observed));
	if (candidate == nullptr || candidate->vaddr != reinterpret_cast<uint64_t>(g_get_game_info))
	{
		std::fputs("NpTrophyBindingTest: observed Trophy export is missing\n", stderr);
		passed = false;
	}
	passed = NegativeIdentity(symbols, observed) && passed;
	auto unknown = Exact("unknown-trophy-binding-nid");
	passed = Missing(symbols, unknown, observed, "NID fallback") && passed;
	if (symbols.Find(Exact("28xmRUFao68")) != nullptr)
	{
		std::fputs("NpTrophyBindingTest: unsupported Trophy Unlock export was registered\n", stderr);
		passed = false;
	}
	if (symbols.FindByStdName(U"YYP3f2W09og") != nullptr ||
	    symbols.FindByStdName(U"sceNpTrophyGetGameInfo") != nullptr ||
	    symbols.FindByStdName(U"sceNpTrophyUnlockTrophy") != nullptr || symbols.StdNameMapSize() != 0)
	{
		std::fputs("NpTrophyBindingTest: encoded identity gained a standard-name fallback\n", stderr);
		passed = false;
	}
	return passed;
}

void Store64(uint8_t* bytes, uint64_t value)
{
	for (size_t byte = 0; byte < 8; ++byte) bytes[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

bool Call(int context, int handle, void* details, void* data, const char* message)
{
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const int result = g_get_game_info(context, handle,
	                                  reinterpret_cast<Trophy::NpTrophyGameDetails*>(details),
	                                  reinterpret_cast<Trophy::NpTrophyGameData*>(data));
	if (result == 0 && *Posix::GetErrorAddr() == ERRNO_SENTINEL) return true;
	std::fprintf(stderr, "NpTrophyBindingTest: %s returned %d, errno=%d\n", message, result, *Posix::GetErrorAddr());
	return false;
}

using Work = std::array<uint8_t, WORK_BYTES>;

void Arm(Work& work, size_t details, size_t data)
{
	work.fill(0xa7);
	if (details < work.size()) Store64(work.data() + details, UINT64_C(0x123456789abcdef0));
	if (data < work.size()) Store64(work.data() + data, UINT64_C(0xfedcba9876543210));
}

void ExpectRecord(Work& expected, size_t offset, size_t bytes)
{
	std::fill(expected.begin() + static_cast<std::ptrdiff_t>(offset + 8),
	          expected.begin() + static_cast<std::ptrdiff_t>(offset + bytes), 0);
}

bool OutputCase(bool details_present, bool data_present, bool adjacent, const char* message)
{
	constexpr size_t details_offset = 17; // Deliberately unaligned.
	const size_t data_offset = adjacent ? details_offset + DETAILS_BYTES : DETAILS_BYTES + 57;
	Work work;
	Arm(work, details_offset, data_offset);
	auto expected = work;
	void* details = details_present ? work.data() + details_offset : nullptr;
	void* data = data_present ? work.data() + data_offset : nullptr;
	if (details_present) ExpectRecord(expected, details_offset, DETAILS_BYTES);
	if (data_present) ExpectRecord(expected, data_offset, DATA_BYTES);
	return Call(1, 1, details, data, message) && Check(work == expected, "output footprint or canary changed");
}

bool TestOutputs()
{
	if (!OutputCase(false, false, false, "both outputs null") ||
	    !OutputCase(true, false, false, "details only") ||
	    !OutputCase(false, true, false, "data only") ||
	    !OutputCase(true, true, false, "distinct outputs") ||
	    !OutputCase(true, true, true, "exactly adjacent outputs")) return false;
	Work repeated;
	constexpr size_t details = 37;
	constexpr size_t data = DETAILS_BYTES + 61;
	for (uint8_t seed : {uint8_t {0x63}, uint8_t {0xd9}})
	{
		repeated.fill(seed);
		Store64(repeated.data() + details, UINT64_C(0x0908070605040302));
		Store64(repeated.data() + data, UINT64_C(0x1020304050607080));
		auto expected = repeated;
		ExpectRecord(expected, details, DETAILS_BYTES);
		ExpectRecord(expected, data, DATA_BYTES);
		if (!Call(1, 1, repeated.data() + details, repeated.data() + data, "repeated stateless query") ||
		    !Check(repeated == expected, "repeated query retained state or wrote unexpected bytes")) return false;
	}
	return true;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Vm::Alloc(0, size, Vm::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Vm::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(m_address + offset); }
	bool Protect(size_t offset, size_t size, Vm::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Vm::Protect(m_address + offset, size, mode);
	}

private:
	size_t m_size;
	uint64_t m_address;
};

bool ProtectedRecord(Mapping& memory, size_t record_size, bool inaccessible_size, const char* message)
{
	if (!Check(memory.Protect(0, 3 * VM_UNIT, Vm::Mode::ReadWrite), "mapping restoration failed")) return false;
	std::memset(memory.Data(), 0xc5, 3 * VM_UNIT);
	const size_t offset = inaccessible_size ? VM_UNIT - 8 : 2 * VM_UNIT - record_size;
	Store64(memory.Data(offset), UINT64_C(0x8877665544332211));
	std::vector<uint8_t> expected(memory.Data(), memory.Data(3 * VM_UNIT));
	std::fill(expected.begin() + static_cast<std::ptrdiff_t>(offset + 8),
	          expected.begin() + static_cast<std::ptrdiff_t>(offset + record_size), 0);
	if (inaccessible_size)
	{
		if (!Check(memory.Protect(0, VM_UNIT, Vm::Mode::NoAccess), "size page was not made inaccessible")) return false;
	} else if (!Check(memory.Protect(2 * VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "guard page protection failed")) return false;
	const bool called = record_size == DETAILS_BYTES
	                        ? Call(1, 1, memory.Data(offset), nullptr, message)
	                        : Call(1, 1, nullptr, memory.Data(offset), message);
	if (!Check(memory.Protect(0, 3 * VM_UNIT, Vm::Mode::ReadWrite), "mapping inspection restoration failed")) return false;
	return called && Check(std::memcmp(memory.Data(), expected.data(), expected.size()) == 0,
	                       "protected record changed size, canary, or guarded boundary");
}

bool TestMemory()
{
	Mapping memory(3 * VM_UNIT);
	return Check(memory.Valid(), "memory fixture allocation failed") &&
	       ProtectedRecord(memory, DETAILS_BYTES, true, "details with inaccessible size") &&
	       ProtectedRecord(memory, DATA_BYTES, true, "data with inaccessible size") &&
	       ProtectedRecord(memory, DETAILS_BYTES, false, "details ending at guard") &&
	       ProtectedRecord(memory, DATA_BYTES, false, "data ending at guard");
}

bool TestConcurrency()
{
	std::atomic<bool> start {false};
	std::atomic<bool> valid {true};
	std::array<std::thread, 4> workers;
	for (auto& worker : workers)
	{
		worker = std::thread([&] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (unsigned iteration = 0; iteration < 16; ++iteration)
				if (!OutputCase(true, true, (iteration & 1U) != 0, "concurrent distinct-buffer query"))
					valid.store(false, std::memory_order_relaxed);
		});
	}
	start.store(true, std::memory_order_release);
	for (auto& worker : workers) worker.join();
	return Check(valid.load(std::memory_order_relaxed), "concurrent distinct outputs were inconsistent");
}

int FatalChild(const char* scenario)
{
	int context = 1;
	int handle = 1;
	Mapping inaccessible(VM_UNIT);
	Mapping read_only(VM_UNIT);
	if (!inaccessible.Valid() || !read_only.Valid()) return 3;
	std::memset(read_only.Data(), 0x6d, VM_UNIT);
	if (!inaccessible.Protect(0, VM_UNIT, Vm::Mode::NoAccess) ||
	    !read_only.Protect(0, VM_UNIT, Vm::Mode::Read)) return 3;
	// Invalid context/handle cases use two valid, non-overlapping NoAccess ranges:
	// any premature input or output access becomes a signal rather than exit 65.
	void* details = inaccessible.Data();
	void* data = inaccessible.Data(VM_UNIT - DATA_BYTES);
	if (std::strcmp(scenario, "context-zero") == 0)
	{
		context = 0;
	} else if (std::strcmp(scenario, "context-min-handle-max") == 0)
	{
		context = std::numeric_limits<int>::min();
		handle = std::numeric_limits<int>::max();
	} else if (std::strcmp(scenario, "context-max") == 0)
	{
		context = std::numeric_limits<int>::max();
	} else if (std::strcmp(scenario, "handle-zero") == 0)
	{
		handle = 0;
	} else if (std::strcmp(scenario, "handle-min") == 0)
	{
		handle = std::numeric_limits<int>::min();
	} else if (std::strcmp(scenario, "handle-max") == 0)
	{
		handle = std::numeric_limits<int>::max();
	} else if (std::strcmp(scenario, "overlap-same") == 0)
	{
		details = read_only.Data(16);
		data = details;
	} else if (std::strcmp(scenario, "overlap-data-in-details") == 0)
	{
		details = read_only.Data(16);
		data = static_cast<uint8_t*>(details) + DETAILS_BYTES - 13;
	} else if (std::strcmp(scenario, "overlap-details-in-data") == 0)
	{
		data = read_only.Data(16);
		details = static_cast<uint8_t*>(data) + 7;
	} else if (std::strcmp(scenario, "overflow-details") == 0)
	{
		details = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - DETAILS_BYTES / 2);
	} else if (std::strcmp(scenario, "overflow-data") == 0)
	{
		data = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - DATA_BYTES / 2);
	} else return 3;
	(void)g_get_game_info(context, handle,
	                     reinterpret_cast<Trophy::NpTrophyGameDetails*>(details),
	                     reinterpret_cast<Trophy::NpTrophyGameData*>(data));
	return 4; // Every selected scenario must terminate through the fatal guard.
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
bool ExpectFatal(const char* executable, const char* scenario)
{
	posix_spawn_file_actions_t actions;
	if (!Check(posix_spawn_file_actions_init(&actions) == 0, "fatal-child actions initialization failed")) return false;
	int error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	if (error == 0) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	pid_t child = 0;
	char* arguments[] = {const_cast<char*>(executable), const_cast<char*>("fatal-child"),
	                     const_cast<char*>(scenario), nullptr};
	if (error == 0) error = posix_spawnp(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (!Check(error == 0, "fatal child could not be spawned")) return false;
	int status = 0;
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR) return Check(false, "fatal child could not be reaped");
	if (WIFEXITED(status) && WEXITSTATUS(status) == 65) return true;
	if (WIFSIGNALED(status))
		std::fprintf(stderr, "NpTrophyBindingTest: fatal scenario %s terminated by signal %d\n",
		             scenario, WTERMSIG(status));
	else if (WIFEXITED(status))
		std::fprintf(stderr, "NpTrophyBindingTest: fatal scenario %s exited %d instead of 65\n",
		             scenario, WEXITSTATUS(status));
	else std::fprintf(stderr, "NpTrophyBindingTest: fatal scenario %s returned an unexpected wait status\n", scenario);
	return false;
}
#endif

bool TestFatalPaths(const char* executable)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	(void)executable;
	// The normal contract remains portable; protected child exit validation
	// uses the POSIX process fixture available on Linux and Apple hosts.
	return true;
#else
	for (const char* scenario : {"context-zero", "context-min-handle-max", "context-max",
	                             "handle-zero", "handle-min", "handle-max", "overlap-same",
	                             "overlap-data-in-details", "overlap-details-in-data",
	                             "overflow-details", "overflow-data"})
		if (!ExpectFatal(executable, scenario)) return false;
	return true;
#endif
}

} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Vm::Init();
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0) return FatalChild(argv[2]);
	if (argc != 1)
	{
		std::fputs("Usage: NpTrophyBindingTest [fatal-child <scenario>]\n", stderr);
		return 2;
	}
	if (!TestBindings() || !TestOutputs()) return 1;
	Network::NetworkSubsystem::Instance()->Init(nullptr);
	if (!Check(Lumen::Libs::ResetSessionState(), "HLE request reset failed") || !TestOutputs()) return 1;
	if (!Check(Network::ResetForNextSession(), "Network reset failed") || !TestOutputs() ||
	    !TestMemory() || !TestConcurrency() || !TestFatalPaths(argv[0])) return 1;
	std::puts("NpTrophyBindingTest: PASS");
	return 0;
}
