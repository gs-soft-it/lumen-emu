#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpInGameMessage.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Libs/Remoteplay.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

namespace Message = Lumen::Libs::NpInGameMessage;
namespace Requests = Lumen::Libs::NpRequests;
namespace Network = Lumen::Libs::Network;
namespace Np = Network::NpManager;
using Result = Message::ModelResult;

constexpr const char* SELECTOR = "LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE";
constexpr uint64_t MAXIMUM = std::numeric_limits<uint64_t>::max();

// Original synthetic checks of the separately specified local experiment.
// These expectations establish no console ABI, result meaning or messaging service.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpInGameMessageTest: %s\n", message);
	return condition;
}

bool State(bool initialized, uint64_t token = 0)
{
	const auto snapshot = Message::GetModelSnapshot();
	return snapshot.initialized == initialized && snapshot.initialization_token == token;
}

bool Live(int request)
{
	Requests::LiveRequestGuard guard(request);
	return guard.IsLive();
}

bool Selectors(bool message, bool other_np)
{
	return Check(Message::ExperimentalEnabled() == message &&
	                 Lumen::Libs::NpPlus::ExperimentalEnabled() == other_np &&
	                 Lumen::Libs::NpParental::ExperimentalEnabled() == other_np &&
	                 !Lumen::Libs::Remoteplay::ExperimentalEnabled(), "independent selectors changed") &&
	       Check(Requests::TrackingEnabled() == other_np, "messaging selection changed NP request tracking");
}

bool Registration(bool message, bool other_np = false)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libNet_1", &symbols), "Net library initialization failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.library = "libSceNpManager";
	exact.library_version = 1;
	exact.module = "libSceNpManager";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	struct Export { const char* nid; uint64_t address; bool enabled; };
	const std::array<Export, 8> exports {{
	    {"GFhVUpRmbHE", reinterpret_cast<uint64_t>(&Message::Initialize), message},
	    {"eiqMCt9UshI", reinterpret_cast<uint64_t>(&Np::NpCreateAsyncRequest), true},
	    {"S7QTn72PrDw", reinterpret_cast<uint64_t>(&Np::NpDeleteRequest), true},
	    {"2rsFmlGWleQ", reinterpret_cast<uint64_t>(&Np::NpCheckNpAvailability), true},
	    {"uqcPJLWL08M", reinterpret_cast<uint64_t>(&Np::NpPollAsync), true},
	    {"eQH7nWPcAgc", reinterpret_cast<uint64_t>(&Np::NpGetState), true},
	    {"r6MyYJkryz8", reinterpret_cast<uint64_t>(&Lumen::Libs::NpPlus::CheckPlus), other_np},
	    {"ilwLM4zOmu4", reinterpret_cast<uint64_t>(&Lumen::Libs::NpParental::GetParentalControlInfo), other_np},
	}};
	for (const auto& expected: exports)
	{
		auto query = exact;
		query.name = expected.nid;
		const auto* found = symbols.Find(query);
		if (!Check(expected.enabled ? found != nullptr && found->vaddr == expected.address : found == nullptr,
		           "NP export selection or binding changed")) return false;
	}
	exact.name = "GFhVUpRmbHE";
	for (int version: {0, 2})
	{
		auto query = exact;
		query.library_version = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported library version resolved")) return false;
		query = exact;
		query.module_version_major = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported module major version resolved")) return false;
		query = exact;
		query.module_version_minor = version;
		if (!Check(symbols.Find(query) == nullptr, "unsupported module minor version resolved")) return false;
	}
	for (const char* name: {"libSceNpManagerForToolkit", "libSceNet"})
	{
		auto query = exact;
		query.library = name;
		if (!Check(symbols.Find(query) == nullptr, "messaging resolved in an unrelated library")) return false;
		query = exact;
		query.module = name;
		if (!Check(symbols.Find(query) == nullptr, "messaging resolved in an unrelated module")) return false;
	}
	auto query = exact;
	query.type = Lumen::Loader::SymbolType::Object;
	if (!Check(symbols.Find(query) == nullptr, "messaging resolved as an object")) return false;
	for (const char* name: {"bMG3cVmUmuk", "sceNpInGameMessageInitialize", "sceNpInGameMessageTerminate", "unknown-message-test-nid"})
	{
		query = exact;
		query.name = name;
		if (!Check(symbols.Find(query) == nullptr, "standard-name or unknown fallback resolved")) return false;
	}
	const uint32_t targets = Lumen::Loader::AuthoritativeHostCallRegistryEnabled()
	                             ? 60U + (message ? 1U : 0U) + (other_np ? 2U : 0U) : 0U;
	return Check(symbols.StdNameMapSize() == 0, "a standard-name alias was registered") &&
	       Check(symbols.HostCallTargetCount() == targets, "unexpected sibling exports were registered");
}

int CreateRequest()
{
	// Existing local async-request fixture; unrelated to the opaque messaging pointer.
	alignas(8) std::array<uint8_t, 24> parameter {};
	parameter[0] = 24;
	parameter[8] = 1;
	parameter[16] = 64;
	return Np::NpCreateAsyncRequest(reinterpret_cast<const Np::NpCreateAsyncRequestParameter*>(parameter.data()));
}

bool LegacyRequests()
{
	if (!Check(!Requests::TrackingEnabled() && Network::ResetForNextSession(), "legacy request setup failed") ||
	    !Check(CreateRequest() == 1 && !Live(1), "messaging enabled request tracking or changed the first ID")) return false;
	uint32_t state = 0xffffffff;
	int polled = -1;
	return Check(Np::NpGetState(9, &state) == 0 && state == 0 && Np::NpPollAsync(1, &polled) == 0 && polled == 0 &&
	                 Np::NpCheckNpAvailability(1, "original-message-test", nullptr) == 0,
	             "existing NP state, polling or availability behavior changed") &&
	       Check(Np::NpDeleteRequest(1) == 0 && Np::NpDeleteRequest(1) == 0, "legacy repeated deletion changed") &&
	       Check(Network::ResetForNextSession(), "legacy request cleanup failed");
}

bool Disabled()
{
	Message::ResetSessionState();
	return Selectors(false, false) && Registration(false) &&
	       Check(Message::InitializeModel(MAXIMUM, nullptr) == Result::Disabled && State(false),
	             "disabled initialization changed state") && LegacyRequests();
}

bool ScalarsAndReset()
{
	for (uint64_t token: {uint64_t(0), uint64_t(1), uint64_t(0x12345678abcdef91), MAXIMUM})
	{
		Message::ResetSessionState();
		if (!Check(State(false), "direct reset retained initialization state") ||
		    !Check(Message::Initialize(token, nullptr) == 0 && State(true, token), "wrapper changed an opaque scalar") ||
		    !Check(Message::InitializeModel(token ^ MAXIMUM, nullptr) == Result::AlreadyInitialized && State(true, token),
		           "duplicate initialization replaced the scalar")) return false;
	}
	if (!Check(Lumen::Libs::ResetSessionState() && State(false), "aggregate reset retained messaging state") ||
	    !Check(Message::InitializeModel(7, nullptr) == Result::Ok && State(true, 7), "initialization after reset failed")) return false;
	Message::ResetSessionState();
	Message::ResetSessionState();
	return Check(State(false), "repeated direct reset retained state");
}

bool RejectedParameters()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "host page size unavailable")) return false;
	void* inaccessible = mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (!Check(inaccessible != MAP_FAILED, "inaccessible fixture mapping failed")) return false;
	std::array<uint8_t, 64> storage {};
	for (size_t byte = 0; byte < storage.size(); ++byte) storage[byte] = static_cast<uint8_t>(19 + byte * 31);
	const auto original = storage;
	const std::array<const void*, 4> parameters {{
	    reinterpret_cast<const void*>(uintptr_t(1)),
	    reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max()),
	    storage.data(), inaccessible,
	}};
	bool passed = true;
	Message::ResetSessionState();
	for (bool initialized: {false, true})
	{
		if (initialized) passed = Check(Message::InitializeModel(13, nullptr) == Result::Ok, "parameter-test setup failed") && passed;
		for (const void* parameter: parameters)
		{
			passed = Check(Message::InitializeModel(MAXIMUM, parameter) == Result::UnsupportedParameter &&
			                   State(initialized, initialized ? 13 : 0) && storage == original,
			               "non-null rejection accessed storage, changed state or lost validation precedence") && passed;
		}
	}
	Message::ResetSessionState();
	return Check(munmap(inaccessible, static_cast<size_t>(page_size)) == 0, "fixture unmap failed") && passed;
}

bool ConcurrentInitialization()
{
	Message::ResetSessionState();
	std::atomic<bool> start {false};
	std::array<Result, 2> results {{Result::Disabled, Result::Disabled}};
	const std::array<uint64_t, 2> tokens {{0, MAXIMUM}};
	std::array<std::thread, 2> callers;
	for (size_t index = 0; index < callers.size(); ++index)
	{
		callers[index] = std::thread([&, index] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Message::InitializeModel(tokens[index], nullptr);
		});
	}
	start.store(true, std::memory_order_release);
	for (auto& caller: callers) caller.join();
	const bool first = results[0] == Result::Ok && results[1] == Result::AlreadyInitialized && State(true, tokens[0]);
	const bool second = results[1] == Result::Ok && results[0] == Result::AlreadyInitialized && State(true, tokens[1]);
	Message::ResetSessionState();
	return Check(first || second, "concurrent initializations did not commit exactly one intact scalar") &&
	       Check(State(false), "reset after joined callers failed");
}

bool Latch(bool expected)
{
	if (!Check(Message::ExperimentalEnabled() == expected, "initial selector disagreed with its expected value") ||
	    !Check(setenv(SELECTOR, expected ? "0" : "1", 1) == 0, "selector mutation failed")) return false;
	Message::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "latch-test aggregate reset failed") ||
	    !Selectors(expected, false) || !Registration(expected)) return false;
	const auto result = Message::InitializeModel(23, nullptr);
	const bool passed = Check(result == (expected ? Result::Ok : Result::Disabled) && State(expected, expected ? 23 : 0),
	                          "environment mutation or reset changed the process latch");
	Message::ResetSessionState();
	return passed && Check(Message::ExperimentalEnabled() == expected && State(false), "direct reset changed the latch");
}

bool Coexist()
{
	if (!Selectors(true, true) || !Registration(true, true) ||
	    !Check(Network::ResetForNextSession(), "coexistence Network reset failed")) return false;
	Message::ResetSessionState();
	if (!Check(CreateRequest() == 1 && CreateRequest() == 2 && Live(1) && Live(2), "shared request setup failed") ||
	    !Check(Message::InitializeModel(31, nullptr) == Result::Ok && Live(1) && Live(2), "initialization invalidated NP requests") ||
	    !Check(Message::InitializeModel(32, nullptr) == Result::AlreadyInitialized &&
	               Message::InitializeModel(33, reinterpret_cast<const void*>(uintptr_t(1))) == Result::UnsupportedParameter &&
	               State(true, 31) && Live(1) && Live(2), "rejected initialization changed existing NP state")) return false;
	Message::ResetSessionState();
	if (!Check(State(false) && Live(1) && Live(2), "messaging reset invalidated shared NP requests") ||
	    !Check(Message::InitializeModel(MAXIMUM, nullptr) == Result::Ok && Np::NpDeleteRequest(1) == 0 &&
	               !Live(1) && Live(2) && State(true, MAXIMUM), "NP deletion changed messaging state")) return false;
	// Live() releases its guard before the reset acquires the shared NP mutex.
	if (!Check(Lumen::Libs::ResetSessionState(), "coexistence aggregate reset failed") ||
	    !Check(State(false) && !Live(1) && !Live(2) && CreateRequest() == 3 && Live(3),
	           "aggregate reset failed to clear liveness or changed the Network allocator")) return false;
	return Check(Network::ResetForNextSession() && !Live(3), "coexistence cleanup retained a live request") && Selectors(true, true);
}

int FatalChild(const char* scenario)
{
	if (std::strcmp(scenario, "disabled") == 0) (void)Message::Initialize(0, nullptr);
	else if (std::strcmp(scenario, "parameter") == 0) (void)Message::Initialize(MAXIMUM, reinterpret_cast<const void*>(uintptr_t(1)));
	else if (std::strcmp(scenario, "duplicate") == 0)
	{
		if (Message::Initialize(0, nullptr) != 0) return 3;
		(void)Message::Initialize(MAXIMUM, nullptr);
	} else return 3;
	return 4;
}

bool ExpectFatal(const char* executable, const char* scenario, const char* outcome)
{
	int descriptors[2] {};
	if (!Check(pipe(descriptors) == 0, "fatal-child pipe failed")) return false;
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) { close(descriptors[0]); close(descriptors[1]); return Check(false, "fatal-child actions failed"); }
	error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
	pid_t child = 0;
	char* arguments[] {const_cast<char*>(executable), const_cast<char*>("fatal-child"), const_cast<char*>(scenario), nullptr};
	if (error == 0) error = posix_spawnp(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(descriptors[1]);
	if (error != 0) { close(descriptors[0]); return Check(false, "fatal-child spawn failed"); }
	std::string captured;
	std::array<char, 1024> buffer {};
	bool read_ok = true;
	for (;;)
	{
		const auto count = read(descriptors[0], buffer.data(), buffer.size());
		if (count > 0) captured.append(buffer.data(), static_cast<size_t>(count));
		else if (count == 0) break;
		else if (errno != EINTR) { read_ok = false; break; }
	}
	close(descriptors[0]);
	int status = 0;
	pid_t waited;
	do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	const std::string marker = std::string("[NP_IN_GAME_MESSAGE_OFFLINE_EXPERIMENT] operation=initialize result=") + outcome +
	                           " experimental=1 console_contract=unverified scalar=opaque parameter=null-only networking=0";
	const std::string diagnostic = std::string("NP in-game messaging offline experiment cannot model initialization: ") + outcome;
	const bool passed = read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 65 &&
	                    captured.find(marker) != std::string::npos && captured.find(diagnostic) != std::string::npos;
	if (!passed) std::fprintf(stderr, "NpInGameMessageTest: fatal scenario %s status=%d output=%s\n", scenario, status, captured.c_str());
	return Check(passed, "unsupported wrapper did not exit explicitly with its specific diagnostic");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		if (setenv(SELECTOR, std::strcmp(argv[2], "disabled") == 0 ? "0" : "1", 1) != 0) return 3;
		for (const char* selector: {"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE", "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE",
		                           "LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE"})
			if (setenv(selector, "0", 1) != 0) return 3;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Network::NetworkSubsystem::Instance()->Init(nullptr);
	bool passed = false;
	if (argc == 2 && std::strcmp(argv[1], "disabled") == 0) passed = Disabled();
	else if (argc == 2 && std::strcmp(argv[1], "enabled") == 0)
		passed = Selectors(true, false) && Registration(true) && ScalarsAndReset() && RejectedParameters() &&
		         ConcurrentInitialization() && LegacyRequests();
	else if (argc == 3 && std::strcmp(argv[1], "latch") == 0 && (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Latch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "coexist") == 0) passed = Coexist();
	else if (argc == 2 && std::strcmp(argv[1], "fatal-paths") == 0)
		passed = ExpectFatal(argv[0], "disabled", "disabled") && ExpectFatal(argv[0], "parameter", "non-null-parameter") &&
		         ExpectFatal(argv[0], "duplicate", "already-initialized");
	else if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0) return FatalChild(argv[2]);
	else
	{
		std::fprintf(stderr, "Usage: NpInGameMessageTest disabled|enabled|latch 0|1|coexist|fatal-paths\n");
		return 2;
	}
	if (!passed) return 1;
	std::puts("NpInGameMessageTest: PASS");
	return 0;
}
