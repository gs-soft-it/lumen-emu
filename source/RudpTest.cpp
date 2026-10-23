#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpInGameMessage.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Libs/Remoteplay.h"
#include "Emulator/Libs/Rudp.h"
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

namespace Rudp = Lumen::Libs::Rudp;
namespace Message = Lumen::Libs::NpInGameMessage;
namespace Requests = Lumen::Libs::NpRequests;
namespace Network = Lumen::Libs::Network;
namespace Np = Network::NpManager;
using Result = Rudp::ModelResult;

constexpr const char* SELECTOR = "LUMEN_EXPERIMENTAL_RUDP_OFFLINE";
constexpr uint64_t MAXIMUM = std::numeric_limits<uint64_t>::max();
constexpr uintptr_t MAX_TOKEN = std::numeric_limits<uintptr_t>::max();
constexpr uint32_t MAX_IO = std::numeric_limits<uint32_t>::max();

// Original fixtures for the approved local profile. They establish no console
// workspace semantics, allocation requirements, result meaning or transport.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "RudpTest: %s\n", message);
	return condition;
}

bool State(bool initialized, uintptr_t workspace = 0, uint64_t value = 0,
           bool io_requested = false, uint32_t io_first = 0, uint32_t io_second = 0)
{
	const auto snapshot = Rudp::GetModelSnapshot();
	return snapshot.initialized == initialized && snapshot.workspace_token == workspace && snapshot.initialization_value == value &&
	       snapshot.io_requested == io_requested && snapshot.io_value_1 == io_first && snapshot.io_value_2 == io_second;
}

bool MessageState(bool initialized, uint64_t value = 0)
{
	const auto snapshot = Message::GetModelSnapshot();
	return snapshot.initialized == initialized && snapshot.initialization_token == value;
}

bool Live(int request)
{
	Requests::LiveRequestGuard guard(request);
	return guard.IsLive();
}

bool Selectors(bool rudp, bool other_np)
{
	return Check(Rudp::ExperimentalEnabled() == rudp && Message::ExperimentalEnabled() == other_np &&
	                 Lumen::Libs::NpPlus::ExperimentalEnabled() == other_np &&
	                 Lumen::Libs::NpParental::ExperimentalEnabled() == other_np &&
	                 !Lumen::Libs::Remoteplay::ExperimentalEnabled(), "independent selectors changed") &&
	       Check(Requests::TrackingEnabled() == other_np, "RUDP selection changed NP request tracking");
}

bool Registration(bool rudp, bool other_np = false)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libNet_1", &symbols), "Net library initialization failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.library = "libSceRudp";
	exact.library_version = 1;
	exact.module = "libSceRudp";
	exact.module_version_major = 0;
	exact.module_version_minor = 0;
	exact.type = Lumen::Loader::SymbolType::Func;
	struct Export { const char* nid; uint64_t address; bool enabled; };
	const std::array<Export, 2> rudp_exports {{
	    {"amuBfI-AQc4", reinterpret_cast<uint64_t>(&Rudp::Init), rudp},
	    {"6PBNpsgyaxw", reinterpret_cast<uint64_t>(&Rudp::EnableInternalIOThread), rudp},
	}};
	for (const auto& expected: rudp_exports)
	{
		exact.name = expected.nid;
		const auto* found = symbols.Find(exact);
		if (!Check(expected.enabled ? found != nullptr && found->vaddr == expected.address : found == nullptr,
		           "RUDP export selection or binding changed")) return false;
		for (int version: {0, 2})
		{
			auto query = exact;
			query.library_version = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported RUDP library version resolved")) return false;
		}
		for (int version: {1, 2})
		{
			auto query = exact;
			query.module_version_major = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported RUDP module major resolved")) return false;
			query = exact;
			query.module_version_minor = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported RUDP module minor resolved")) return false;
		}
		for (const char* name: {"libSceNet", "libSceNpManager"})
		{
			auto query = exact;
			query.library = name;
			if (!Check(symbols.Find(query) == nullptr, "RUDP resolved in an unrelated library")) return false;
			query = exact;
			query.module = name;
			if (!Check(symbols.Find(query) == nullptr, "RUDP resolved in an unrelated module")) return false;
		}
		auto query = exact;
		query.type = Lumen::Loader::SymbolType::Object;
		if (!Check(symbols.Find(query) == nullptr, "RUDP resolved as an object")) return false;
	}
	auto query = exact;
	for (const char* name: {"3hBvwqEwqj8", "sBlSzFh-J5c", "sceRudpInit", "sceRudpEnd", "sceRudpEnableInternalIOThread",
	                       "sceRudpDisableInternalIOThread", "original-rudp-test-unknown-nid"})
	{
		query = exact;
		query.name = name;
		if (!Check(symbols.Find(query) == nullptr, "End, Disable, standard-name or unknown fallback resolved")) return false;
	}
	query = exact;
	query.library = "libSceNpManager";
	query.module = "libSceNpManager";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	const std::array<Export, 8> exports {{
	    {"eiqMCt9UshI", reinterpret_cast<uint64_t>(&Np::NpCreateAsyncRequest), true},
	    {"S7QTn72PrDw", reinterpret_cast<uint64_t>(&Np::NpDeleteRequest), true},
	    {"2rsFmlGWleQ", reinterpret_cast<uint64_t>(&Np::NpCheckNpAvailability), true},
	    {"uqcPJLWL08M", reinterpret_cast<uint64_t>(&Np::NpPollAsync), true},
	    {"eQH7nWPcAgc", reinterpret_cast<uint64_t>(&Np::NpGetState), true},
	    {"GFhVUpRmbHE", reinterpret_cast<uint64_t>(&Message::Initialize), other_np},
	    {"r6MyYJkryz8", reinterpret_cast<uint64_t>(&Lumen::Libs::NpPlus::CheckPlus), other_np},
	    {"ilwLM4zOmu4", reinterpret_cast<uint64_t>(&Lumen::Libs::NpParental::GetParentalControlInfo), other_np},
	}};
	for (const auto& expected: exports)
	{
		query.name = expected.nid;
		const auto* found = symbols.Find(query);
		if (!Check(expected.enabled ? found != nullptr && found->vaddr == expected.address : found == nullptr,
		           "RUDP registration changed an NP export")) return false;
	}
	const uint32_t targets = Lumen::Loader::AuthoritativeHostCallRegistryEnabled()
	                             ? 60U + (rudp ? 2U : 0U) + (other_np ? 3U : 0U) : 0U;
	return Check(symbols.StdNameMapSize() == 0 && symbols.HostCallTargetCount() == targets,
	             "unexpected alias or sibling HostCall target was registered");
}

int CreateRequest()
{
	// Existing local async-request fixture, independent of the opaque RUDP inputs.
	alignas(8) std::array<uint8_t, 24> parameter {};
	parameter[0] = 24;
	parameter[8] = 1;
	parameter[16] = 64;
	return Np::NpCreateAsyncRequest(reinterpret_cast<const Np::NpCreateAsyncRequestParameter*>(parameter.data()));
}

bool LegacyRequests()
{
	if (!Check(!Requests::TrackingEnabled() && Network::ResetForNextSession(), "legacy request setup failed") ||
	    !Check(CreateRequest() == 1 && !Live(1), "RUDP enabled tracking or changed the first NP request ID")) return false;
	uint32_t state = 0xffffffff;
	int polled = -1;
	return Check(Np::NpGetState(9, &state) == 0 && state == 0 && Np::NpPollAsync(1, &polled) == 0 && polled == 0 &&
	                 Np::NpCheckNpAvailability(1, "original-rudp-test", nullptr) == 0,
	             "legacy NP state, polling or availability changed") &&
	       Check(Np::NpDeleteRequest(1) == 0 && Np::NpDeleteRequest(1) == 0, "legacy repeated deletion changed") &&
	       Check(Network::ResetForNextSession(), "legacy request cleanup failed");
}

bool Disabled()
{
	Rudp::ResetSessionState();
	return Selectors(false, false) && Registration(false) &&
	       Check(Rudp::InitializeModel(reinterpret_cast<const void*>(MAX_TOKEN), MAXIMUM) == Result::Disabled &&
	                 Rudp::InitializeModel(nullptr, 0) == Result::Disabled &&
	                 Rudp::EnableInternalIOThreadModel(MAX_IO, MAX_IO) == Result::Disabled && State(false),
	             "disabled operation changed state or lost precedence over missing Init") && LegacyRequests();
}

bool OpaqueInputsAndReset()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "host page size unavailable")) return false;
	void* inaccessible = mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (!Check(inaccessible != MAP_FAILED, "inaccessible fixture mapping failed")) return false;
	std::array<uint8_t, 64> storage {};
	for (size_t byte = 0; byte < storage.size(); ++byte) storage[byte] = static_cast<uint8_t>(29 + byte * 37);
	const auto original = storage;
	const std::array<const void*, 6> pointers {{nullptr, reinterpret_cast<const void*>(uintptr_t(1)),
	    reinterpret_cast<const void*>(MAX_TOKEN), reinterpret_cast<const void*>(uintptr_t(0x12345678abcdef91)),
	    storage.data(), inaccessible}};
	bool passed = true;
	for (const void* pointer: pointers)
	{
		for (uint64_t value: {uint64_t(0), uint64_t(1), uint64_t(0x91abcdef12345678), MAXIMUM})
		{
			Rudp::ResetSessionState();
			passed = Check(State(false) && Rudp::Init(pointer, value) == 0 &&
			                   State(true, reinterpret_cast<uintptr_t>(pointer), value) && storage == original,
			               "initialization accessed memory or changed an opaque input") && passed;
			passed = Check(Rudp::InitializeModel(reinterpret_cast<const void*>(uintptr_t(3)), value ^ MAXIMUM) ==
			                   Result::AlreadyInitialized && State(true, reinterpret_cast<uintptr_t>(pointer), value) && storage == original,
			               "duplicate initialization changed retained inputs or guest storage") && passed;
		}
	}
	passed = Check(Lumen::Libs::ResetSessionState() && State(false), "aggregate reset retained RUDP state") && passed;
	passed = Check(Rudp::InitializeModel(nullptr, 7) == Result::Ok && State(true, 0, 7), "initialization after reset failed") && passed;
	Rudp::ResetSessionState();
	Rudp::ResetSessionState();
	return Check(State(false) && storage == original, "repeated reset retained state or changed storage") &&
	       Check(munmap(inaccessible, static_cast<size_t>(page_size)) == 0, "fixture unmap failed") && passed;
}

bool IOInputsAndReset()
{
	const std::array<std::array<uint32_t, 2>, 5> values {{{{0, 0}}, {{1, MAX_IO}}, {{MAX_IO, 1}},
	                                                  {{0x91abcdef, 0x12345678}}, {{MAX_IO, MAX_IO}}}};
	for (const auto& pair: values)
	{
		Rudp::ResetSessionState();
		if (!Check(State(false) && Rudp::EnableInternalIOThreadModel(pair[0], pair[1]) == Result::NotInitialized && State(false),
		           "I/O request before Init changed state") ||
		    !Check(Rudp::InitializeModel(reinterpret_cast<const void*>(uintptr_t(1)), MAXIMUM) == Result::Ok && State(true, 1, MAXIMUM),
		           "I/O fixture initialization failed") ||
		    !Check(Rudp::EnableInternalIOThread(pair[0], pair[1]) == 0 && State(true, 1, MAXIMUM, true, pair[0], pair[1]),
		           "I/O request changed an opaque scalar or Init state") ||
		    !Check(Rudp::EnableInternalIOThreadModel(pair[0] ^ MAX_IO, pair[1] ^ MAX_IO) == Result::AlreadyEnabled &&
		               Rudp::InitializeModel(nullptr, 0) == Result::AlreadyInitialized && State(true, 1, MAXIMUM, true, pair[0], pair[1]),
		           "duplicate Enable or Init replaced an existing I/O request")) return false;
	}
	if (!Check(Lumen::Libs::ResetSessionState() && State(false) &&
	               Rudp::EnableInternalIOThreadModel(1, 2) == Result::NotInitialized && State(false),
	           "aggregate reset retained an I/O request or bypassed Init") ||
	    !Check(Rudp::InitializeModel(nullptr, 3) == Result::Ok && Rudp::EnableInternalIOThreadModel(4, 5) == Result::Ok &&
	               State(true, 0, 3, true, 4, 5), "fresh session could not request I/O")) return false;
	Rudp::ResetSessionState();
	Rudp::ResetSessionState();
	return Check(State(false), "direct reset retained I/O fields");
}

bool ConcurrentInitialization()
{
	Rudp::ResetSessionState();
	std::atomic<bool> start {false};
	std::array<Result, 2> results {{Result::Disabled, Result::Disabled}};
	const std::array<uintptr_t, 2> tokens {{0, MAX_TOKEN}};
	const std::array<uint64_t, 2> values {{MAXIMUM, 0}};
	std::array<std::thread, 2> callers;
	for (size_t index = 0; index < callers.size(); ++index)
		callers[index] = std::thread([&, index] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Rudp::InitializeModel(reinterpret_cast<const void*>(tokens[index]), values[index]);
		});
	start.store(true, std::memory_order_release);
	for (auto& caller: callers) caller.join();
	const bool first = results[0] == Result::Ok && results[1] == Result::AlreadyInitialized && State(true, tokens[0], values[0]);
	const bool second = results[1] == Result::Ok && results[0] == Result::AlreadyInitialized && State(true, tokens[1], values[1]);
	Rudp::ResetSessionState();
	return Check((first || second) && State(false), "concurrent initialization failed to commit exactly one intact input pair");
}

bool ConcurrentIORequests()
{
	Rudp::ResetSessionState();
	if (!Check(Rudp::InitializeModel(nullptr, 19) == Result::Ok, "concurrent I/O fixture initialization failed")) return false;
	std::atomic<bool> start {false};
	std::array<Result, 2> results {{Result::Disabled, Result::Disabled}};
	const std::array<std::array<uint32_t, 2>, 2> values {{{{0, MAX_IO}}, {{MAX_IO, 0}}}};
	std::array<std::thread, 2> callers;
	for (size_t index = 0; index < callers.size(); ++index)
		callers[index] = std::thread([&, index] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Rudp::EnableInternalIOThreadModel(values[index][0], values[index][1]);
		});
	start.store(true, std::memory_order_release);
	for (auto& caller: callers) caller.join();
	const bool first = results[0] == Result::Ok && results[1] == Result::AlreadyEnabled && State(true, 0, 19, true, 0, MAX_IO);
	const bool second = results[1] == Result::Ok && results[0] == Result::AlreadyEnabled && State(true, 0, 19, true, MAX_IO, 0);
	Rudp::ResetSessionState();
	return Check((first || second) && State(false), "concurrent I/O requests failed to retain exactly one coherent pair");
}

bool Latch(bool expected)
{
	if (!Check(Rudp::ExperimentalEnabled() == expected, "initial selector disagreed with the expected value") ||
	    !Check(setenv(SELECTOR, expected ? "0" : "1", 1) == 0, "selector mutation failed")) return false;
	Rudp::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "latch-test aggregate reset failed") ||
	    !Selectors(expected, false) || !Registration(expected)) return false;
	const auto result = Rudp::InitializeModel(reinterpret_cast<const void*>(uintptr_t(11)), 23);
	if (!Check(result == (expected ? Result::Ok : Result::Disabled) && State(expected, expected ? 11 : 0, expected ? 23 : 0),
	           "environment mutation or reset changed the Init latch")) return false;
	const auto io_result = Rudp::EnableInternalIOThreadModel(9, 31);
	const bool passed = Check(io_result == (expected ? Result::Ok : Result::Disabled) &&
	                              State(expected, expected ? 11 : 0, expected ? 23 : 0, expected, expected ? 9 : 0, expected ? 31 : 0),
	                          "environment mutation or reset changed the I/O latch");
	Rudp::ResetSessionState();
	return passed && Check(Rudp::ExperimentalEnabled() == expected && State(false), "direct reset changed the latch");
}

bool Coexist()
{
	if (!Selectors(true, true) || !Registration(true, true) ||
	    !Check(Network::ResetForNextSession(), "coexistence Network reset failed")) return false;
	Rudp::ResetSessionState();
	Message::ResetSessionState();
	if (!Check(CreateRequest() == 1 && CreateRequest() == 2 && Live(1) && Live(2) &&
	               Message::InitializeModel(77, nullptr) == Message::ModelResult::Ok, "coexistence fixture setup failed") ||
	    !Check(Rudp::EnableInternalIOThreadModel(1, 2) == Result::NotInitialized && State(false) &&
	               MessageState(true, 77) && Live(1) && Live(2), "premature I/O request changed another service") ||
	    !Check(Rudp::InitializeModel(reinterpret_cast<const void*>(uintptr_t(1)), 42) == Result::Ok &&
	               Rudp::EnableInternalIOThreadModel(5, 6) == Result::Ok &&
	               Rudp::EnableInternalIOThreadModel(0, 0) == Result::AlreadyEnabled &&
	               Rudp::InitializeModel(nullptr, MAXIMUM) == Result::AlreadyInitialized && State(true, 1, 42, true, 5, 6) &&
	               MessageState(true, 77) && Live(1) && Live(2), "RUDP operations changed NP or messaging state")) return false;
	Rudp::ResetSessionState();
	if (!Check(State(false) && MessageState(true, 77) && Live(1) && Live(2), "RUDP reset changed NP or messaging state") ||
	    !Check(Rudp::InitializeModel(reinterpret_cast<const void*>(MAX_TOKEN), 73) == Result::Ok &&
	               Rudp::EnableInternalIOThreadModel(7, 8) == Result::Ok, "RUDP reinitialization or I/O request failed")) return false;
	Message::ResetSessionState();
	if (!Check(MessageState(false) && State(true, MAX_TOKEN, 73, true, 7, 8) && Live(1) && Live(2), "messaging reset changed RUDP or NP state") ||
	    !Check(Message::InitializeModel(88, nullptr) == Message::ModelResult::Ok && Np::NpDeleteRequest(1) == 0 &&
	               !Live(1) && Live(2) && State(true, MAX_TOKEN, 73, true, 7, 8) && MessageState(true, 88), "NP deletion changed service state")) return false;
	// Each Live() releases its guard before aggregate reset acquires the NP mutex.
	if (!Check(Lumen::Libs::ResetSessionState() && State(false) && MessageState(false) && !Live(1) && !Live(2) &&
	               CreateRequest() == 3 && Live(3), "aggregate reset retained service state or changed the Network allocator")) return false;
	return Check(Network::ResetForNextSession() && !Live(3), "coexistence cleanup retained a request") && Selectors(true, true);
}

int FatalChild(const char* scenario)
{
	if (std::strcmp(scenario, "disabled") == 0) (void)Rudp::Init(nullptr, 0);
	else if (std::strcmp(scenario, "duplicate") == 0)
	{
		if (Rudp::Init(nullptr, 0) != 0) return 3;
		(void)Rudp::Init(reinterpret_cast<const void*>(MAX_TOKEN), MAXIMUM);
	} else if (std::strcmp(scenario, "io-disabled") == 0 || std::strcmp(scenario, "io-before-init") == 0)
		(void)Rudp::EnableInternalIOThread(MAX_IO, MAX_IO);
	else if (std::strcmp(scenario, "io-duplicate") == 0)
	{
		if (Rudp::Init(nullptr, 0) != 0 || Rudp::EnableInternalIOThread(0, 0) != 0) return 3;
		(void)Rudp::EnableInternalIOThread(MAX_IO, MAX_IO);
	} else return 3;
	return 4;
}

bool ExpectFatal(const char* executable, const char* scenario, const char* outcome, bool io = false)
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
	const std::string marker = std::string("[RUDP_OFFLINE_EXPERIMENT] operation=") +
	    (io ? "enable-internal-io" : "initialize") + " result=" + outcome +
	    " experimental=1 console_contract=unverified " +
	    (io ? "scalars=opaque host_thread=0 networking=0" : "workspace=opaque-token scalar=opaque networking=0");
	const std::string diagnostic = std::string("RUDP offline experiment cannot model ") +
	    (io ? "internal I/O request: " : "initialization: ") + outcome;
	const bool passed = read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 65 &&
	                    captured.find(marker) != std::string::npos && captured.find(diagnostic) != std::string::npos;
	if (!passed) std::fprintf(stderr, "RudpTest: fatal scenario %s status=%d output=%s\n", scenario, status, captured.c_str());
	return Check(passed, "unsupported wrapper did not exit explicitly with its specific diagnostic");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		const bool disabled = std::strcmp(argv[2], "disabled") == 0 || std::strcmp(argv[2], "io-disabled") == 0;
		if (setenv(SELECTOR, disabled ? "0" : "1", 1) != 0) return 3;
		for (const char* selector: {"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE", "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE",
		                           "LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE", "LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE"})
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
		passed = Selectors(true, false) && Registration(true) && OpaqueInputsAndReset() && IOInputsAndReset() &&
		         ConcurrentInitialization() && ConcurrentIORequests() && LegacyRequests();
	else if (argc == 3 && std::strcmp(argv[1], "latch") == 0 && (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Latch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "coexist") == 0) passed = Coexist();
	else if (argc == 2 && std::strcmp(argv[1], "fatal-paths") == 0)
		passed = ExpectFatal(argv[0], "disabled", "disabled") && ExpectFatal(argv[0], "duplicate", "already-initialized") &&
		         ExpectFatal(argv[0], "io-disabled", "disabled", true) &&
		         ExpectFatal(argv[0], "io-before-init", "not-initialized", true) &&
		         ExpectFatal(argv[0], "io-duplicate", "already-requested", true);
	else if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0) return FatalChild(argv[2]);
	else
	{
		std::fprintf(stderr, "Usage: RudpTest disabled|enabled|latch 0|1|coexist|fatal-paths\n");
		return 2;
	}
	if (!passed) return 1;
	std::puts("RudpTest: PASS");
	return 0;
}
