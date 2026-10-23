#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Remoteplay.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {

namespace Remoteplay = Lumen::Libs::Remoteplay;
using Result         = Remoteplay::ModelResult;
using Phase          = Remoteplay::Phase;

constexpr const char* SELECTOR = "LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE";

// These cases exercise Lumen's explicitly limited offline model. They do not
// claim to establish the console's service contract or its error values. The
// fixtures and expected transitions are original and use no external tests.
bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "RemoteplayTest: %s\n", message);
	}
	return condition;
}

bool HasState(Phase phase, uint64_t address = 0, uint64_t size = 0)
{
	const auto snapshot = Remoteplay::GetModelSnapshot();
	return snapshot.phase == phase && snapshot.workspace_address == address && snapshot.workspace_size == size;
}

bool TestRegistration(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libRemoteplay_1", &symbols) == enabled,
	           "library selection does not follow the experimental selector"))
	{
		return false;
	}
	struct ExpectedExport
	{
		const char* nid;
		uint64_t    address;
	};
	const std::array<ExpectedExport, 4> exports {{
	    {"k1SwgkMSOM8", reinterpret_cast<uint64_t>(&Remoteplay::Initialize)},
	    {"xQeIryTX7dY", reinterpret_cast<uint64_t>(&Remoteplay::Approve)},
	    {"mrNh78tBpmg", reinterpret_cast<uint64_t>(&Remoteplay::Prohibit)},
	    {"BOwybKVa3Do", reinterpret_cast<uint64_t>(&Remoteplay::Terminate)},
	}};
	Lumen::Loader::SymbolResolve query {};
	query.library              = "libSceRemoteplay";
	query.library_version      = 1;
	query.module               = "libSceRemoteplay";
	query.module_version_major = 0;
	query.module_version_minor = 0;
	query.type                 = Lumen::Loader::SymbolType::Func;
	for (const auto& expected: exports)
	{
		query.name = expected.nid;
		const auto* record = symbols.Find(query);
		if (!Check(enabled ? record != nullptr && record->vaddr == expected.address : record == nullptr,
		           "an administrative NID is missing, incorrectly bound, or exposed while disabled"))
		{
			return false;
		}
		query.module_version_major = 1;
		if (!Check(symbols.Find(query) == nullptr, "an unsupported module version resolved"))
		{
			return false;
		}
		query.module_version_major = 0;
	}
	query.name = "unknown-remoteplay-test-nid";
	return Check(symbols.Find(query) == nullptr, "an unknown NID resolved") &&
	       Check(symbols.StdNameMapSize() == 0, "the experiment registered standard-name aliases") &&
	       Check(symbols.HostCallTargetCount() ==
	                 (enabled && Lumen::Loader::AuthoritativeHostCallRegistryEnabled() ? 4U : 0U),
	             "the experiment registered an unexpected number of host targets");
}

bool TestDisabled()
{
	Remoteplay::ResetSessionState();
	return Check(!Remoteplay::ExperimentalEnabled(), "disabled mode enabled the experiment") &&
	       TestRegistration(false) &&
	       Check(Remoteplay::InitializeModel(1, 1) == Result::Disabled, "disabled initialization was admitted") &&
	       Check(Remoteplay::SetApprovalModel(true) == Result::Disabled, "disabled approval was admitted") &&
	       Check(Remoteplay::SetApprovalModel(false) == Result::Disabled, "disabled prohibition was admitted") &&
	       Check(Remoteplay::TerminateModel() == Result::Disabled, "disabled termination was admitted") &&
	       Check(HasState(Phase::Stopped), "disabled operations retained workspace or changed phase");
}

bool TestStateMachine()
{
	Remoteplay::ResetSessionState();
	if (!Check(HasState(Phase::Stopped), "reset did not clear all model state") ||
	    !Check(Remoteplay::SetApprovalModel(true) == Result::NotInitialized, "approval before initialization was admitted") ||
	    !Check(Remoteplay::SetApprovalModel(false) == Result::NotInitialized, "prohibition before initialization was admitted") ||
	    !Check(Remoteplay::TerminateModel() == Result::NotInitialized, "termination before initialization was admitted") ||
	    !Check(HasState(Phase::Stopped), "rejected operations modified the stopped state"))
	{
		return false;
	}
	constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
	const std::array<std::array<uint64_t, 2>, 5> invalid_workspaces {{
	    {{0, 0}}, {{0, 1}}, {{1, 0}}, {{maximum, 1}}, {{maximum - 15, 16}},
	}};
	for (const auto& workspace: invalid_workspaces)
	{
		if (!Check(Remoteplay::InitializeModel(workspace[0], workspace[1]) == Result::InvalidWorkspace,
		           "an empty or overflowing workspace range was admitted") ||
		    !Check(HasState(Phase::Stopped), "invalid initialization modified model state"))
		{
			return false;
		}
	}
	// An address is an opaque token here: even this non-dereferenceable range
	// is accepted by the model without accessing or owning host memory.
	if (!Check(Remoteplay::InitializeModel(maximum - 1, 1) == Result::Ok,
	           "the highest non-overflowing one-byte token was rejected") ||
	    !Check(HasState(Phase::Prohibited, maximum - 1, 1), "initialization did not start prohibited") ||
	    !Check(Remoteplay::InitializeModel(1, 1) == Result::AlreadyInitialized, "duplicate initialization was admitted") ||
	    !Check(HasState(Phase::Prohibited, maximum - 1, 1), "duplicate initialization replaced the workspace") ||
	    !Check(Remoteplay::SetApprovalModel(true) == Result::Ok, "approval failed") ||
	    !Check(Remoteplay::SetApprovalModel(true) == Result::Ok, "repeated approval failed") ||
	    !Check(HasState(Phase::Approved, maximum - 1, 1), "approval changed the workspace") ||
	    !Check(Remoteplay::InitializeModel(2, 2) == Result::AlreadyInitialized, "approved state admitted initialization") ||
	    !Check(HasState(Phase::Approved, maximum - 1, 1), "rejected initialization changed approval") ||
	    !Check(Remoteplay::SetApprovalModel(false) == Result::Ok, "prohibition failed") ||
	    !Check(Remoteplay::SetApprovalModel(false) == Result::Ok, "repeated prohibition failed") ||
	    !Check(HasState(Phase::Prohibited, maximum - 1, 1), "prohibition changed the workspace") ||
	    !Check(Remoteplay::TerminateModel() == Result::Ok, "termination failed") ||
	    !Check(HasState(Phase::Stopped), "termination retained model state") ||
	    !Check(Remoteplay::TerminateModel() == Result::NotInitialized, "repeated termination was admitted"))
	{
		return false;
	}
	return Check(Remoteplay::InitializeModel(1, 1) == Result::Ok, "initialization after termination failed") &&
	       Check(HasState(Phase::Prohibited, 1, 1), "a new initialization inherited the previous session") &&
	       Check(Remoteplay::TerminateModel() == Result::Ok, "second lifecycle termination failed");
}

bool TestWrappersAndWorkspace()
{
	std::array<uint8_t, 192> workspace {};
	for (size_t i = 0; i < workspace.size(); ++i)
	{
		workspace[i] = static_cast<uint8_t>(i * 37 + 11);
	}
	const auto original = workspace;
	void* token = workspace.data() + 32;
	const auto address = reinterpret_cast<uint64_t>(token);
	constexpr uint64_t size = 128;
	Remoteplay::ResetSessionState();
	if (!Check(Remoteplay::Initialize(token, size) == 0, "valid wrapper initialization failed") ||
	    !Check(HasState(Phase::Prohibited, address, size), "wrapper initialization did not reach the model") ||
	    !Check(Remoteplay::Approve() == 0 && Remoteplay::Approve() == 0, "wrapper approval was not idempotent") ||
	    !Check(HasState(Phase::Approved, address, size), "wrapper approval did not reach the model") ||
	    !Check(Remoteplay::Prohibit() == 0 && Remoteplay::Prohibit() == 0, "wrapper prohibition was not idempotent") ||
	    !Check(HasState(Phase::Prohibited, address, size), "wrapper prohibition did not reach the model") ||
	    !Check(Remoteplay::Terminate() == 0, "wrapper termination failed") ||
	    !Check(HasState(Phase::Stopped), "wrapper termination retained the workspace") ||
	    !Check(workspace == original, "administrative wrappers wrote to the workspace or its guards") ||
	    !Check(Remoteplay::Initialize(token, size) == 0, "wrapper initialization after termination failed") ||
	    !Check(Remoteplay::Approve() == 0, "approval before aggregate reset failed") ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed") ||
	    !Check(HasState(Phase::Stopped), "aggregate HLE reset retained Remoteplay state") ||
	    !Check(workspace == original, "aggregate HLE reset wrote to caller-owned workspace"))
	{
		return false;
	}
	return Check(Remoteplay::Initialize(token, size) == 0, "wrapper initialization after aggregate reset failed") &&
	       Check(HasState(Phase::Prohibited, address, size), "aggregate reset preserved old approval") &&
	       Check(Remoteplay::Terminate() == 0, "final wrapper termination failed") &&
	       Check(workspace == original, "repeated lifecycle modified caller-owned workspace");
}

bool TestConcurrentApproval()
{
	constexpr uint64_t address = 0x18345;
	constexpr uint64_t size = 0x927;
	Remoteplay::ResetSessionState();
	if (!Check(Remoteplay::InitializeModel(address, size) == Result::Ok, "concurrency setup failed"))
	{
		return false;
	}
	std::atomic<bool> start {false};
	std::atomic<bool> valid {true};
	std::array<std::thread, 4> workers;
	for (size_t worker = 0; worker < workers.size(); ++worker)
	{
		workers[worker] = std::thread([worker, &start, &valid]
		{
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			for (size_t iteration = 0; iteration < 500; ++iteration)
			{
				const auto result = Remoteplay::SetApprovalModel((worker + iteration) % 2 == 0);
				const auto snapshot = Remoteplay::GetModelSnapshot();
				if (result != Result::Ok || snapshot.workspace_address != address || snapshot.workspace_size != size ||
				    (snapshot.phase != Phase::Approved && snapshot.phase != Phase::Prohibited))
				{
					valid.store(false, std::memory_order_relaxed);
				}
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers)
	{
		worker.join();
	}
	return Check(valid.load(std::memory_order_relaxed), "concurrent transitions exposed inconsistent state") &&
	       Check(Remoteplay::SetApprovalModel(false) == Result::Ok && HasState(Phase::Prohibited, address, size),
	             "serial transition after concurrent calls failed") &&
	       Check(Lumen::Libs::ResetSessionState(), "aggregate reset after joined callers failed") &&
	       Check(HasState(Phase::Stopped), "joined callers left state after aggregate reset");
}

bool TestLatch(bool expected)
{
	if (!Check(Remoteplay::ExperimentalEnabled() == expected, "initial selector does not match latch expectation") ||
	    !Check(setenv(SELECTOR, expected ? "0" : "1", 1) == 0, "could not change the selector for the latch test") ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate reset during latch test failed") ||
	    !Check(Remoteplay::ExperimentalEnabled() == expected, "session reset or environment change altered the process latch") ||
	    !TestRegistration(expected))
	{
		return false;
	}
	const auto result = Remoteplay::InitializeModel(1, 1);
	Remoteplay::ResetSessionState();
	return Check(result == (expected ? Result::Ok : Result::Disabled), "model ignored the process latch") &&
	       Check(Remoteplay::ExperimentalEnabled() == expected, "direct model reset altered the process latch") &&
	       Check(HasState(Phase::Stopped), "direct model reset retained state during latch test");
}

} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	bool passed = false;
	if (argc == 2 && std::strcmp(argv[1], "disabled") == 0)
	{
		passed = TestDisabled();
	} else if (argc == 2 && std::strcmp(argv[1], "enabled") == 0)
	{
		passed = Check(Remoteplay::ExperimentalEnabled(), "enabled mode did not enable the experiment") &&
		         TestRegistration(true) && TestStateMachine() && TestWrappersAndWorkspace() && TestConcurrentApproval();
	} else if (argc == 3 && std::strcmp(argv[1], "latch") == 0 &&
	           (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
	{
		passed = TestLatch(std::strcmp(argv[2], "1") == 0);
	} else
	{
		std::fprintf(stderr, "Usage: RemoteplayTest disabled|enabled|latch 0|1\n");
		return 2;
	}
	if (!passed)
	{
		return 1;
	}
	std::puts("RemoteplayTest: PASS");
	return 0;
}
