#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Ult.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

extern char** environ;

namespace {

namespace Ult = Lumen::Libs::Ult;
using ScalarType = Lumen::Loader::HostCallScalarType;
using InitializeFunction = int32_t (LUMEN_SYSV_ABI*)();

static_assert(sizeof(int32_t) == 4);
static_assert(std::is_signed_v<int32_t>);
static_assert(std::is_same_v<decltype(&Ult::Initialize), InitializeFunction>);

constexpr char SELECTOR[] = "LUMEN_EXPERIMENTAL_ULT_INITIALIZE";
constexpr char FIRST_DIAGNOSTIC[] =
    "[ULT_INITIALIZE_LOCAL_EXPERIMENT] result=complete-local call=first experimental=1 "
    "console_contract=unverified resources=0 guest_memory=0\n";
constexpr char REPEAT_DIAGNOSTIC[] =
    "[ULT_INITIALIZE_LOCAL_EXPERIMENT] result=complete-local call=repeat experimental=1 "
    "console_contract=unverified resources=0 guest_memory=0\n";
constexpr char DISABLED_DIAGNOSTIC[] =
    "[ULT_INITIALIZE_LOCAL_EXPERIMENT] result=disabled experimental=0 "
    "console_contract=unverified resources=0 guest_memory=0\n";

// These fixtures exercise only Lumen's approved local profile. They are
// original and make no assertion about console behavior.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "UltInitializeTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve UltIdentity(const char* nid = "hZIg1EWGsHM")
{
	Lumen::Loader::SymbolResolve query {};
	query.name                  = nid;
	query.library               = "libSceUlt";
	query.library_version       = 1;
	query.module                = "libSceUlt";
	query.module_version_major  = 1;
	query.module_version_minor  = 1;
	query.type                  = Lumen::Loader::SymbolType::Func;
	return query;
}

template <typename Operation>
bool CaptureStderr(std::string* output, Operation operation)
{
	output->clear();
	std::fflush(stderr);
	std::FILE* capture = std::tmpfile();
	if (!Check(capture != nullptr, "could not create stderr capture")) return false;

	const int saved_stderr = ::dup(STDERR_FILENO);
	if (saved_stderr < 0 || ::dup2(::fileno(capture), STDERR_FILENO) < 0)
	{
		if (saved_stderr >= 0) ::close(saved_stderr);
		std::fclose(capture);
		return Check(false, "could not redirect stderr");
	}

	operation();
	std::fflush(stderr);
	const bool restored = ::dup2(saved_stderr, STDERR_FILENO) >= 0;
	::close(saved_stderr);
	if (!restored || std::fseek(capture, 0, SEEK_SET) != 0)
	{
		std::fclose(capture);
		return Check(false, "could not restore or rewind stderr");
	}

	std::array<char, 512> buffer {};
	for (;;)
	{
		const size_t size = std::fread(buffer.data(), 1, buffer.size(), capture);
		output->append(buffer.data(), size);
		if (size != buffer.size()) break;
	}
	const bool read_ok = std::ferror(capture) == 0;
	std::fclose(capture);
	return Check(read_ok, "could not read stderr capture");
}

size_t Count(const std::string& text, const char* needle)
{
	size_t result = 0;
	size_t offset = 0;
	const size_t length = std::strlen(needle);
	while ((offset = text.find(needle, offset)) != std::string::npos)
	{
		++result;
		offset += length;
	}
	return result;
}

const Lumen::Loader::SymbolRecord* FindUlt(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(UltIdentity());
}

bool CheckRegistration(Lumen::Loader::SymbolDatabase* symbols, bool enabled)
{
	const bool selected = Lumen::Libs::Init(U"libUlt_1", symbols);
	const auto* record = FindUlt(*symbols);
	if (!Check(selected == enabled, "library selector returned the wrong result") ||
	    !Check(enabled ? record != nullptr && record->vaddr == reinterpret_cast<uint64_t>(&Ult::Initialize)
	                   : record == nullptr,
	           "exact binding presence or target does not match the selector"))
	{
		return false;
	}
	return Check(enabled ? symbols->HostCallTargetCount() == 1 : symbols->HostCallTargetCount() == 0,
	             "isolated ULT registration published an unexpected target count");
}

bool TestSelector(const char* expected_value, bool enabled)
{
	const char* actual = std::getenv(SELECTOR);
	if (expected_value == nullptr)
	{
		if (!Check(actual == nullptr, "CTest did not remove the selector")) return false;
	} else if (!Check(actual != nullptr && std::strcmp(actual, expected_value) == 0,
	                  "CTest provided the wrong selector value"))
	{
		return false;
	}
	if (!Check(Ult::ExperimentalEnabled() == enabled, "exact-one selector result is wrong")) return false;
	Lumen::Loader::SymbolDatabase symbols;
	return CheckRegistration(&symbols, enabled);
}

bool TestLatch(bool enabled)
{
	const char* value = std::getenv(SELECTOR);
	if (!Check(value != nullptr && std::strcmp(value, enabled ? "1" : "0") == 0,
	           "latch test received the wrong initial selector") ||
	    !Check(Ult::ExperimentalEnabled() == enabled, "initial latch value is wrong"))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase first;
	if (!CheckRegistration(&first, enabled) ||
	    !Check(::setenv(SELECTOR, enabled ? "0" : "1", 1) == 0,
	           "could not change the environment after latching"))
	{
		return false;
	}
	Ult::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate reset failed during latch test") ||
	    !Check(Ult::ExperimentalEnabled() == enabled, "reset or environment change altered the process latch"))
	{
		return false;
	}
	Lumen::Loader::SymbolDatabase second;
	if (!CheckRegistration(&second, enabled)) return false;
	if (!enabled) return true;

	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Ult::Initialize(); }) &&
	       Check(result == 0, "latched enabled wrapper did not return signed32 zero") &&
	       Check(output == FIRST_DIAGNOSTIC, "latched enabled wrapper did not begin a new local session");
}

bool TestExactIdentity()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;
	const auto exact = UltIdentity();

	for (int version: {0, 2})
	{
		auto wrong = exact;
		wrong.library_version = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library version resolved")) return false;
		wrong = exact;
		wrong.module_version_major = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module major version resolved")) return false;
		wrong = exact;
		wrong.module_version_minor = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module minor version resolved")) return false;
	}

	for (const char* other: {"Ult", "libUlt", "SceUlt", "libSceFiber", "libSceJobManager", "libkernel"})
	{
		auto wrong = exact;
		wrong.library = other;
		if (!Check(symbols.Find(wrong) == nullptr, "library namespace alias resolved")) return false;
		wrong = exact;
		wrong.module = other;
		if (!Check(symbols.Find(wrong) == nullptr, "module namespace alias resolved")) return false;
	}

	for (auto type: {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
	                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
	{
		auto wrong = exact;
		wrong.type = type;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong symbol type resolved")) return false;
	}

	for (const char* name: {"sceUltInitialize", "sceUltFinalize", "sceUltCreateFiber",
	                        "synthetic-unapproved-ult-sibling"})
	{
		if (!Check(symbols.Find(UltIdentity(name)) == nullptr, "an alias or unapproved sibling resolved")) return false;
	}
	if (!Check(symbols.StdNameMapSize() == 0 && symbols.FindByStdName(U"sceUltInitialize") == nullptr &&
	               symbols.FindByStdName(U"hZIg1EWGsHM") == nullptr,
	           "ULT gained a standard-name or NID alias"))
	{
		return false;
	}

	for (const char32_t* id: {U"libSceUlt_1", U"libUlt_0", U"libUlt_2"})
	{
		Lumen::Loader::SymbolDatabase wrong_library;
		if (!Check(!Lumen::Libs::Init(id, &wrong_library) && FindUlt(wrong_library) == nullptr,
		           "an unsupported loader library identifier was accepted"))
		{
			return false;
		}
	}

	for (const char32_t* id: {U"libFiber_1", U"libJobManager_1", U"libkernel_1"})
	{
		Lumen::Loader::SymbolDatabase service;
		if (!Check(Lumen::Libs::Init(id, &service), "existing independent service did not initialize") ||
		    !Check(FindUlt(service) == nullptr, "Fiber, JobManager or pthread registered ULT implicitly"))
		{
			return false;
		}
	}
	return true;
}

bool TestScalarAbi()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;
	const auto* record = FindUlt(symbols);
	if (record == nullptr) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 0 && metadata->result == ScalarType::Gp &&
	               metadata->invoker != nullptr,
	           "zero-argument scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (auto argument: metadata->arguments)
		if (!Check(argument == ScalarType::Void, "zero-argument metadata contains an argument")) return false;

	std::array<uint64_t, 8> canaries {
	    UINT64_C(0x1029384756abcdef), UINT64_C(0xfedcba6547382910),
	    UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211),
	    UINT64_C(0x01030507090b0d0f), UINT64_C(0xf0d0b09070503010),
	    UINT64_C(0x55aa55aa33cc33cc), UINT64_C(0xcc33cc33aa55aa55),
	};
	const auto original = canaries;
	uint64_t invoked = UINT64_MAX;
	int32_t direct = -1;
	Ult::ResetSessionState();
	std::string first;
	std::string repeat;
	if (!CaptureStderr(&first, [&] { invoked = metadata->invoker(canaries.data()); }) ||
	    !CaptureStderr(&repeat, [&] { direct = Ult::Initialize(); }))
	{
		return false;
	}
	return Check(invoked == 0 && direct == int32_t {0}, "scalar invoker or direct wrapper did not return zero") &&
	       Check(canaries == original, "zero-argument invoker read as output or mutated synthetic guest canaries") &&
	       Check(first == FIRST_DIAGNOSTIC && repeat == REPEAT_DIAGNOSTIC,
	             "scalar invoker did not use the first/repeat wrapper path");
}

bool TestFirstRepeat()
{
	Ult::ResetSessionState();
	std::array<int32_t, 4> results {-1, -1, -1, -1};
	std::string output;
	if (!CaptureStderr(&output, [&] {
		for (auto& result: results) result = Ult::Initialize();
	}))
	{
		return false;
	}
	for (int32_t result: results)
		if (!Check(result == 0, "serial initialization did not return signed32 zero")) return false;
	const std::string expected = std::string(FIRST_DIAGNOSTIC) + REPEAT_DIAGNOSTIC +
	                             REPEAT_DIAGNOSTIC + REPEAT_DIAGNOSTIC;
	return Check(output == expected, "serial first/repeat diagnostics are not exact or bounded");
}

bool TestConcurrency()
{
	constexpr size_t WORKERS = 16;
	Ult::ResetSessionState();
	std::atomic<bool> start {false};
	std::array<int32_t, WORKERS> results {};
	results.fill(-1);
	std::array<std::thread, WORKERS> workers;
	std::string output;
	if (!CaptureStderr(&output, [&] {
		for (size_t index = 0; index < workers.size(); ++index)
		{
			workers[index] = std::thread([&, index] {
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				results[index] = Ult::Initialize();
			});
		}
		start.store(true, std::memory_order_release);
		for (auto& worker: workers) worker.join();
	}))
	{
		return false;
	}
	for (int32_t result: results)
		if (!Check(result == 0, "concurrent initialization did not return zero")) return false;
	if (!Check(Count(output, FIRST_DIAGNOSTIC) == 1 && Count(output, REPEAT_DIAGNOSTIC) == WORKERS - 1,
	           "concurrent calls did not elect exactly one first observer"))
	{
		return false;
	}
	const size_t expected_bytes = std::strlen(FIRST_DIAGNOSTIC) +
	                              (WORKERS - 1) * std::strlen(REPEAT_DIAGNOSTIC);
	if (!Check(output.size() == expected_bytes, "concurrent diagnostics contained extra or partial data")) return false;
	int32_t after = -1;
	return CaptureStderr(&output, [&] { after = Ult::Initialize(); }) &&
	       Check(after == 0 && output == REPEAT_DIAGNOSTIC,
	             "post-concurrency call did not observe repeat");
}

bool CallAndExpect(const char* expected)
{
	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Ult::Initialize(); }) &&
	       Check(result == 0, "lifecycle call did not return signed32 zero") &&
	       Check(output == expected, "lifecycle call emitted the wrong observation");
}

bool TestResetLifecycle()
{
	Ult::ResetSessionState();
	Ult::ResetSessionState();
	if (!CallAndExpect(FIRST_DIAGNOSTIC) || !CallAndExpect(REPEAT_DIAGNOSTIC)) return false;

	Ult::ResetSessionState();
	Ult::ResetSessionState();
	if (!CallAndExpect(FIRST_DIAGNOSTIC) ||
	    !Check(::setenv(SELECTOR, "0", 1) == 0, "could not change selector before aggregate reset") ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed") ||
	    !Check(Ult::ExperimentalEnabled(), "aggregate reset re-read the latched selector") ||
	    !CallAndExpect(FIRST_DIAGNOSTIC) ||
	    !Check(Lumen::Libs::ResetSessionState(), "repeated aggregate HLE reset failed") ||
	    !CallAndExpect(FIRST_DIAGNOSTIC))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase services;
	if (!Check(Lumen::Libs::Init(U"libFiber_1", &services) &&
	               Lumen::Libs::Init(U"libJobManager_1", &services) &&
	               Lumen::Libs::Init(U"libkernel_1", &services),
	           "independent service fixtures did not register"))
	{
		return false;
	}
	const uint32_t targets_before = services.HostCallTargetCount();
	const int thread_before = Lumen::Libs::LibKernel::PthreadGetthreadid();
	if (!CallAndExpect(REPEAT_DIAGNOSTIC)) return false;
	const int thread_after = Lumen::Libs::LibKernel::PthreadGetthreadid();
	return Check(services.HostCallTargetCount() == targets_before && FindUlt(services) == nullptr,
	             "ULT invocation mutated or joined an independent service registry") &&
	       Check(thread_before == thread_after, "ULT invocation changed the current pthread identity");
}

bool TestInitAll(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	Lumen::Libs::InitAll(&symbols);
	const auto* record = FindUlt(symbols);
	if (!Check(symbols.HostCallRegistrySealed(), "InitAll did not seal the authoritative registry")) return false;
	if (!enabled)
		return Check(record == nullptr, "InitAll exposed ULT while disabled");
	return Check(record != nullptr && record->vaddr == reinterpret_cast<uint64_t>(&Ult::Initialize),
	             "InitAll omitted or misbound the exact ULT export") &&
	       Check(symbols.HostCallScalarMetadataFor(record->vaddr) != nullptr,
	             "InitAll omitted ULT scalar metadata");
}

bool TestFatalDisabled(const char* executable)
{
	int descriptors[2] {};
	if (!Check(::pipe(descriptors) == 0, "fatal-child pipe failed")) return false;
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);

	pid_t child = 0;
	char* arguments[] {const_cast<char*>(executable), const_cast<char*>("fatal-child"), nullptr};
	if (error == 0) error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	::close(descriptors[1]);
	if (error != 0)
	{
		::close(descriptors[0]);
		return Check(false, "fatal child could not be spawned");
	}

	std::string output;
	std::array<char, 512> buffer {};
	bool read_ok = true;
	for (;;)
	{
		const ssize_t size = ::read(descriptors[0], buffer.data(), buffer.size());
		if (size > 0) output.append(buffer.data(), static_cast<size_t>(size));
		else if (size == 0) break;
		else if (errno != EINTR)
		{
			read_ok = false;
			break;
		}
	}
	::close(descriptors[0]);
	int status = 0;
	pid_t waited;
	do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	return Check(read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff),
	             "disabled wrapper did not use the explicit fatal exit") &&
	       Check(Count(output, DISABLED_DIAGNOSTIC) == 1 &&
	                 output.find("ULT Initialize local experiment is disabled") != std::string::npos,
	             "disabled wrapper omitted its bounded specific diagnostic") &&
	       Check(output.size() < 4096 && output.find("0x") == std::string::npos,
	             "disabled diagnostic was unbounded or exposed an address");
}

bool RunCase(const char* name, const char* executable)
{
	if (std::strcmp(name, "selector-absent") == 0) return TestSelector(nullptr, false);
	if (std::strcmp(name, "selector-zero") == 0) return TestSelector("0", false);
	if (std::strcmp(name, "selector-empty") == 0) return TestSelector("", false);
	if (std::strcmp(name, "selector-malformed") == 0) return TestSelector("01", false);
	if (std::strcmp(name, "selector-whitespace") == 0) return TestSelector("1 ", false);
	if (std::strcmp(name, "selector-word") == 0) return TestSelector("true", false);
	if (std::strcmp(name, "selector-two") == 0) return TestSelector("2", false);
	if (std::strcmp(name, "selector-one") == 0) return TestSelector("1", true);
	if (std::strcmp(name, "latch-disabled") == 0) return TestLatch(false);
	if (std::strcmp(name, "latch-enabled") == 0) return TestLatch(true);
	if (std::strcmp(name, "exact-identity") == 0) return TestExactIdentity();
	if (std::strcmp(name, "scalar-abi") == 0) return TestScalarAbi();
	if (std::strcmp(name, "first-repeat") == 0) return TestFirstRepeat();
	if (std::strcmp(name, "concurrency") == 0) return TestConcurrency();
	if (std::strcmp(name, "reset-lifecycle") == 0) return TestResetLifecycle();
	if (std::strcmp(name, "init-all-disabled") == 0) return TestInitAll(false);
	if (std::strcmp(name, "init-all-enabled") == 0) return TestInitAll(true);
	if (std::strcmp(name, "fatal-disabled") == 0) return TestFatalDisabled(executable);
	return Check(false, "unknown test case");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		(void)Ult::Initialize();
		return 4;
	}

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!Check(argc == 2, "expected exactly one test case") || !RunCase(argv[1], argv[0])) return 1;
	std::puts("UltInitializeTest: PASS");
	return 0;
}
