#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpRequests.h"
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
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

namespace Parental = Lumen::Libs::NpParental;
namespace Plus = Lumen::Libs::NpPlus;
namespace Requests = Lumen::Libs::NpRequests;
namespace Network = Lumen::Libs::Network;
namespace Np = Network::NpManager;
namespace Vm = Lumen::Core::VirtualMemory;
using Result = Parental::ModelResult;

constexpr const char* PLUS_SELECTOR = "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE";
constexpr const char* PARENTAL_SELECTOR = "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE";
constexpr size_t IDENTIFIER = 65; // Deliberately unaligned.
constexpr size_t AGE = 160;
constexpr size_t RESTRICTIONS = 193;
constexpr size_t PLUS_DESCRIPTOR = 256;
constexpr size_t PLUS_RESULT = 352;

// Original synthetic tests of the separately chosen restricted local profile.
// Neither the test fixtures nor their expected values qualify a console ABI,
// account identity, real age or parental-control service.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpParentalTest: %s\n", message);
	return condition;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Vm::Alloc(0, size, Vm::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Vm::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	size_t Size() const { return m_size; }
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }
	bool Protect(size_t offset, size_t size, Vm::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Vm::Protect(Address(offset), size, mode);
	}
	bool Free()
	{
		if (!Valid() || !Vm::Free(m_address)) return false;
		m_address = 0;
		return true;
	}

private:
	size_t m_size;
	uint64_t m_address;
};

void Store64(uint8_t* bytes, uint64_t value)
{
	for (size_t byte = 0; byte < 8; ++byte) bytes[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

void Fill(Mapping& memory)
{
	for (size_t byte = 0; byte < memory.Size(); ++byte) memory.Data()[byte] = static_cast<uint8_t>(109 + byte * 17);
	Store64(memory.Data(PLUS_DESCRIPTOR), 56);
}

void ArmOutputs(Mapping& memory)
{
	memory.Data()[AGE] = 0xca;
	for (size_t byte = 0; byte < 3; ++byte) memory.Data()[RESTRICTIONS + byte] = static_cast<uint8_t>(0xd1 + byte);
}

std::vector<uint8_t> Snapshot(const Mapping& memory)
{
	return {memory.Data(), memory.Data() + memory.Size()};
}

bool Matches(const Mapping& memory, const std::vector<uint8_t>& expected)
{
	return std::memcmp(memory.Data(), expected.data(), expected.size()) == 0;
}

bool Reject(Mapping& memory, int32_t request, uint64_t identifier, uint64_t age, uint64_t restrictions,
            Result expected, const char* message)
{
	ArmOutputs(memory);
	const auto before = Snapshot(memory);
	return Check(Parental::QueryModel(request, identifier, age, restrictions) == expected, message) &&
	       Check(Matches(memory, before), "validation rejection changed input, another output or a guard");
}

bool Complete(Mapping& memory, int request, uint64_t identifier, size_t age = AGE,
              size_t restrictions = RESTRICTIONS, bool wrapper = false)
{
	memory.Data()[age] = 0xb9;
	for (size_t byte = 0; byte < 3; ++byte) memory.Data()[restrictions + byte] = static_cast<uint8_t>(0xe1 + byte);
	auto expected = Snapshot(memory);
	expected[age] = 0;
	for (size_t byte = 0; byte < 3; ++byte) expected[restrictions + byte] = 1;
	const bool completed = wrapper ? Parental::GetParentalControlInfo(request, reinterpret_cast<const void*>(identifier),
	                                                                memory.Data(age), memory.Data(restrictions)) == 0
	                               : Parental::QueryModel(request, identifier, memory.Address(age), memory.Address(restrictions)) == Result::Ok;
	return Check(completed, "supported parental query did not complete locally") &&
	       Check(Matches(memory, expected), "query did not write exactly age=0 and restrictions={1,1,1}");
}

int CreateRequest()
{
	// This original fixture describes only the existing in-tree async creation
	// parameter: size, CPU mask, priority and padding. It is not a getter record.
	alignas(8) std::array<uint8_t, 24> parameter {};
	Store64(parameter.data(), parameter.size());
	Store64(parameter.data() + 8, 1);
	parameter[16] = 64;
	return Np::NpCreateAsyncRequest(reinterpret_cast<const Np::NpCreateAsyncRequestParameter*>(parameter.data()));
}

bool Selectors(bool plus, bool parental)
{
	return Check(Plus::ExperimentalEnabled() == plus && Parental::ExperimentalEnabled() == parental,
	             "service selectors are not independent exact-value process latches") &&
	       Check(Requests::Enabled(Requests::Service::Plus) == plus &&
	                 Requests::Enabled(Requests::Service::Parental) == parental && Requests::TrackingEnabled() == (plus || parental),
	             "shared request tracking does not follow the selected services");
}

bool Registration(bool plus, bool parental)
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
	struct Export { const char* nid; uint64_t address; };
	const std::array<Export, 14> existing {{
	    {"3Zl8BePTh9Y", reinterpret_cast<uint64_t>(&Np::NpCheckCallback)},
	    {"Ec63y59l9tw", reinterpret_cast<uint64_t>(&Np::NpSetNpTitleId)},
	    {"A2CQ3kgSopQ", reinterpret_cast<uint64_t>(&Np::NpSetContentRestriction)},
	    {"VfRSmPmj8Q8", reinterpret_cast<uint64_t>(&Np::NpRegisterStateCallback)},
	    {"uFJpaKNBAj4", reinterpret_cast<uint64_t>(&Np::NpRegisterGamePresenceCallback)},
	    {"GImICnh+boA", reinterpret_cast<uint64_t>(&Np::NpRegisterPlusEventCallback)},
	    {"hw5KNqAAels", reinterpret_cast<uint64_t>(&Np::NpRegisterNpReachabilityStateCallback)},
	    {"p-o74CnoNzY", reinterpret_cast<uint64_t>(&Np::NpGetNpId)},
	    {"XDncXQIJUSk", reinterpret_cast<uint64_t>(&Np::NpGetOnlineId)},
	    {"eiqMCt9UshI", reinterpret_cast<uint64_t>(&Np::NpCreateAsyncRequest)},
	    {"S7QTn72PrDw", reinterpret_cast<uint64_t>(&Np::NpDeleteRequest)},
	    {"2rsFmlGWleQ", reinterpret_cast<uint64_t>(&Np::NpCheckNpAvailability)},
	    {"uqcPJLWL08M", reinterpret_cast<uint64_t>(&Np::NpPollAsync)},
	    {"eQH7nWPcAgc", reinterpret_cast<uint64_t>(&Np::NpGetState)},
	}};
	for (const auto& expected: existing)
	{
		auto query = exact;
		query.name = expected.nid;
		const auto* found = symbols.Find(query);
		if (!Check(found != nullptr && found->vaddr == expected.address, "an existing NP export was changed")) return false;
	}
	for (bool is_parental: {false, true})
	{
		auto query = exact;
		query.name = is_parental ? "ilwLM4zOmu4" : "r6MyYJkryz8";
		const uint64_t address = is_parental ? reinterpret_cast<uint64_t>(&Parental::GetParentalControlInfo)
		                                     : reinterpret_cast<uint64_t>(&Plus::CheckPlus);
		const bool enabled = is_parental ? parental : plus;
		const auto* found = symbols.Find(query);
		if (!Check(enabled ? found != nullptr && found->vaddr == address : found == nullptr,
		           "service export selection or function binding is incorrect")) return false;
		const auto selected = query;
		for (int version: {0, 2})
		{
			query = selected;
			query.library_version = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported library version resolved")) return false;
			query = selected;
			query.module_version_major = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported module major version resolved")) return false;
			query = selected;
			query.module_version_minor = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported module minor version resolved")) return false;
		}
		for (const char* other: {"libSceNpManagerForToolkit", "libSceNet", "libSceUnknownParentalTest"})
		{
			query = selected;
			query.library = other;
			if (!Check(symbols.Find(query) == nullptr, "service resolved in an unrelated library")) return false;
			query = selected;
			query.module = other;
			if (!Check(symbols.Find(query) == nullptr, "service resolved in an unrelated module")) return false;
		}
		query = selected;
		query.type = Lumen::Loader::SymbolType::Object;
		if (!Check(symbols.Find(query) == nullptr, "service resolved as an object")) return false;
	}
	for (const char* name: {"unknown-parental-test-nid", "sceNpGetParentalControlInfo", "sceNpCheckPlus"})
	{
		auto query = exact;
		query.name = name;
		if (!Check(symbols.Find(query) == nullptr, "unknown or standard-name fallback resolved")) return false;
	}
	// The existing Net library has 60 host functions. Each selected experiment
	// adds exactly its one reviewed export, without an additional sibling API.
	const uint32_t expected_targets = Lumen::Loader::AuthoritativeHostCallRegistryEnabled()
	                                      ? 60U + (plus ? 1U : 0U) + (parental ? 1U : 0U) : 0U;
	Lumen::Loader::SymbolDatabase remoteplay;
	return Check(symbols.StdNameMapSize() == 0 && symbols.FindByStdName(U"sceNpGetParentalControlInfo") == nullptr,
	             "a standard-name alias was registered") &&
	       Check(symbols.HostCallTargetCount() == expected_targets, "unexpected host exports were registered") &&
	       Check(!Lumen::Libs::Init(U"libRemoteplay_1", &remoteplay), "NP selectors enabled independent Remoteplay exports");
}

bool Services(Mapping& memory, int request, bool live, bool plus, bool parental)
{
	if (parental && live)
	{
		if (!Complete(memory, request, memory.Address(IDENTIFIER), AGE, RESTRICTIONS, true)) return false;
	} else if (!Reject(memory, request, memory.Address(IDENTIFIER), memory.Address(AGE), memory.Address(RESTRICTIONS),
	                   parental ? Result::InvalidRequest : Result::Disabled, "parental liveness or disabled policy changed")) return false;
	memory.Data()[PLUS_RESULT] = 0xa7;
	auto expected = Snapshot(memory);
	const auto result = Plus::CheckModel(request, memory.Address(PLUS_DESCRIPTOR), memory.Address(PLUS_RESULT));
	const auto wanted = !plus ? Plus::ModelResult::Disabled : live ? Plus::ModelResult::Ok : Plus::ModelResult::InvalidRequest;
	if (plus && live) expected[PLUS_RESULT] = 0;
	return Check(result == wanted && Matches(memory, expected), "shared request state or parental selection changed the Plus query");
}

bool TestMatrix(Mapping& memory, bool plus, bool parental)
{
	if (!Selectors(plus, parental) || !Registration(plus, parental) ||
	    !Check(Network::ResetForNextSession(), "matrix setup reset failed") ||
	    !Services(memory, 1, false, plus, parental) || !Check(CreateRequest() == 1, "first request was not ID 1") ||
	    !Services(memory, 1, true, plus, parental)) return false;
	uint32_t state = 0xdeadbeef;
	int polled = -1;
	if (!Check(Np::NpGetState(9, &state) == 0 && state == 0, "existing NP state behavior changed") ||
	    !Check(Np::NpPollAsync(1, &polled) == 0 && polled == 0, "existing NP poll behavior changed")) return false;
	const bool tracking = plus || parental;
	if (tracking && (!Check(CreateRequest() == 2 && CreateRequest() == 3, "either selected service must allow multiple issued IDs") ||
	                 !Services(memory, 2, true, plus, parental) || !Services(memory, 3, true, plus, parental))) return false;
	if (!Check(Np::NpDeleteRequest(1) == 0 && Np::NpDeleteRequest(1) == 0, "repeated legacy deletion changed") ||
	    !Services(memory, 1, false, plus, parental)) return false;
	polled = -1;
	if (!Check(Np::NpPollAsync(1, &polled) == 0 && polled == 0 &&
	               Np::NpCheckNpAvailability(1, "original-parental-test", nullptr) == 0,
	           "existing issued-but-deleted non-Plus behavior changed")) return false;
	if (tracking)
	{
		if (!Services(memory, 2, true, plus, parental) || !Services(memory, 3, true, plus, parental) ||
		    !Check(Np::NpDeleteRequest(2) == 0 && Np::NpDeleteRequest(2) == 0, "second request deletion failed") ||
		    !Services(memory, 2, false, plus, parental) || !Services(memory, 3, true, plus, parental) ||
		    !Check(CreateRequest() == 4, "request deletion recycled an ID") || !Services(memory, 4, true, plus, parental) ||
		    !Services(memory, 5, false, plus, parental)) return false;
		polled = -1;
		if (!Check(Np::NpPollAsync(2, &polled) == 0 && polled == 0 &&
		               Np::NpCheckNpAvailability(2, "original-parental-test", nullptr) == 0,
		           "additional issued-but-deleted non-Plus behavior changed")) return false;
	}
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed")) return false;
	for (int request: {1, 2, 3, 4})
		if (!Services(memory, request, false, plus, parental)) return false;
	if (tracking && (!Check(CreateRequest() == 5, "aggregate reset changed the Network allocator") ||
	                 !Services(memory, 5, true, plus, parental))) return false;
	if (!Check(Network::ResetForNextSession(), "Network reset failed") || !Services(memory, 5, false, plus, parental) ||
	    !Check(CreateRequest() == 1, "Network reset did not restart allocation at 1") || !Services(memory, 1, true, plus, parental) ||
	    !Services(memory, 2, false, plus, parental)) return false;
	Requests::ResetSessionState();
	return Services(memory, 1, false, plus, parental) && Check(Network::ResetForNextSession(), "matrix final reset failed");
}

bool TestMemory(Mapping& memory, size_t page_size)
{
	Fill(memory);
	if (!Check(CreateRequest() == 1, "memory-test request creation failed")) return false;
	const uint64_t identifier = memory.Address(IDENTIFIER);
	const uint64_t age = memory.Address(AGE);
	const uint64_t restrictions = memory.Address(RESTRICTIONS);
	for (int32_t invalid: {std::numeric_limits<int32_t>::min(), -1, 0, 2, std::numeric_limits<int32_t>::max()})
		if (!Reject(memory, invalid, identifier, age, restrictions, Result::InvalidRequest, "non-live parental request was admitted")) return false;
	for (unsigned mask = 1; mask < 8; ++mask)
	{
		const auto expected = (mask & 1U) != 0 ? Result::InvalidIdentifier : (mask & 2U) != 0 ? Result::InvalidAgeOutput
		                                                                                                : Result::InvalidRestrictionsOutput;
		if (!Reject(memory, 1, (mask & 1U) != 0 ? 0 : identifier, (mask & 2U) != 0 ? 0 : age,
		            (mask & 4U) != 0 ? 0 : restrictions, expected, "null-pointer combination was admitted")) return false;
	}
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "unmapped fixture allocation failed")) return false;
	const uint64_t unmapped = freed.Address();
	if (!Check(freed.Free(), "unmapped fixture release failed")) return false;
	constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
	for (uint64_t invalid: {uint64_t(1), unmapped, maximum, maximum - 18})
		if (!Reject(memory, 1, invalid, age, restrictions, Result::InvalidIdentifier, "unmapped or overflowing identifier was admitted")) return false;
	for (uint64_t invalid: {uint64_t(1), unmapped, maximum})
		if (!Reject(memory, 1, identifier, invalid, restrictions, Result::InvalidAgeOutput, "invalid age span was admitted")) return false;
	for (uint64_t invalid: {uint64_t(1), unmapped, maximum, maximum - 1})
		if (!Reject(memory, 1, identifier, age, invalid, Result::InvalidRestrictionsOutput, "invalid restrictions span was admitted")) return false;
	for (uint8_t pattern: {uint8_t(0), uint8_t(0xff), uint8_t(0x7f), uint8_t(0x81)})
	{
		std::memset(memory.Data(IDENTIFIER), pattern, 20);
		if (!Complete(memory, 1, identifier, AGE, RESTRICTIONS, true) || !Complete(memory, 1, identifier)) return false;
	}
	// Every identifier byte is opaque, including the candidate terminator and
	// filler positions; unaligned and non-terminated inputs remain admitted.
	for (size_t byte = 0; byte < 20; ++byte) memory.Data()[IDENTIFIER + byte] = static_cast<uint8_t>(37 + byte * 9);
	if (!Complete(memory, 1, identifier)) return false;
	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "protected fixture allocation failed")) return false;
	Fill(protected_memory);
	const auto original = Snapshot(protected_memory);
	for (auto mode: {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		if (!Check(protected_memory.Protect(0, page_size, mode), "fixture protection failed")) return false;
		if (mode == Vm::Mode::Read)
		{
			if (!Complete(memory, 1, protected_memory.Address(IDENTIFIER))) return false;
		} else if (!Reject(memory, 1, protected_memory.Address(IDENTIFIER), age, restrictions, Result::InvalidIdentifier,
		                   "inaccessible identifier was admitted")) return false;
		if (!Reject(memory, 1, identifier, protected_memory.Address(AGE), restrictions, Result::InvalidAgeOutput,
		            "protected age output was admitted") ||
		    !Reject(memory, 1, identifier, age, protected_memory.Address(RESTRICTIONS), Result::InvalidRestrictionsOutput,
		            "protected restrictions output was admitted or changed age first") ||
		    !Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "fixture protection restore failed") ||
		    !Check(Matches(protected_memory, original), "protected caller storage changed")) return false;
	}
	return true;
}

bool TestBoundaries(Mapping& memory, size_t page_size)
{
	const uint64_t identifier = memory.Address(IDENTIFIER);
	const uint64_t age = memory.Address(AGE);
	const uint64_t restrictions = memory.Address(RESTRICTIONS);
	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "boundary allocation failed")) return false;
	Fill(boundary);
	for (size_t readable: {size_t(1), size_t(16), size_t(19)})
	{
		const auto before = Snapshot(boundary);
		if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "identifier boundary protection failed") ||
		    !Reject(memory, 1, boundary.Address(page_size - readable), age, restrictions, Result::InvalidIdentifier,
		            "truncated 20-byte identifier was admitted") ||
		    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "identifier boundary restore failed") ||
		    !Check(Matches(boundary, before), "rejected identifier boundary changed storage")) return false;
	}
	const auto identifier_before = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "exact identifier boundary protection failed") ||
	    !Complete(memory, 1, boundary.Address(page_size - 20)) ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "exact identifier boundary restore failed") ||
	    !Check(Matches(boundary, identifier_before), "identifier read modified its allocation")) return false;
	for (bool age_at_boundary: {true, false})
	{
		ArmOutputs(memory);
		Fill(boundary);
		auto main_expected = Snapshot(memory);
		auto boundary_expected = Snapshot(boundary);
		const size_t bytes = age_at_boundary ? 1 : 3;
		if (age_at_boundary)
		{
			boundary_expected[page_size - 1] = 0;
			for (size_t byte = 0; byte < 3; ++byte) main_expected[RESTRICTIONS + byte] = 1;
		} else
		{
			main_expected[AGE] = 0;
			for (size_t byte = 0; byte < 3; ++byte) boundary_expected[page_size - 3 + byte] = 1;
		}
		if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "exact output boundary protection failed") ||
		    !Check(Parental::QueryModel(1, identifier, age_at_boundary ? boundary.Address(page_size - bytes) : age,
		                               age_at_boundary ? restrictions : boundary.Address(page_size - bytes)) == Result::Ok,
		           "exact one-byte or three-byte output ending at a protected page was rejected") ||
		    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "exact output boundary restore failed") ||
		    !Check(Matches(memory, main_expected) && Matches(boundary, boundary_expected), "exact output boundary changed other bytes")) return false;
	}
	for (size_t writable: {size_t(0), size_t(1), size_t(2)})
	{
		Fill(boundary);
		const auto before = Snapshot(boundary);
		if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "restrictions boundary protection failed") ||
		    !Reject(memory, 1, identifier, age, boundary.Address(page_size - writable), Result::InvalidRestrictionsOutput,
		            "truncated restrictions output was admitted or changed age first") ||
		    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "restrictions boundary restore failed") ||
		    !Check(Matches(boundary, before), "rejected restrictions changed their writable prefix")) return false;
	}
	const auto before = Snapshot(boundary);
	return Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "age boundary protection failed") &&
	       Reject(memory, 1, identifier, boundary.Address(page_size), restrictions, Result::InvalidAgeOutput,
	              "inaccessible boundary age output was admitted") &&
	       Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "age boundary restore failed") &&
	       Check(Matches(boundary, before), "rejected age boundary changed storage");
}

bool TestAliases(Mapping& memory)
{
	const uint64_t identifier = memory.Address(IDENTIFIER);
	for (size_t offset: {size_t(0), size_t(1), size_t(2)})
		if (!Reject(memory, 1, identifier, memory.Address(RESTRICTIONS + offset), memory.Address(RESTRICTIONS),
		            Result::OverlappingOutputs, "overlapping outputs were admitted")) return false;
	if (!Complete(memory, 1, identifier, RESTRICTIONS - 1, RESTRICTIONS) ||
	    !Complete(memory, 1, identifier, RESTRICTIONS + 3, RESTRICTIONS)) return false;
	for (size_t offset: {size_t(0), size_t(7), size_t(19)})
		if (!Complete(memory, 1, identifier, IDENTIFIER + offset, RESTRICTIONS)) return false;
	for (size_t offset: {size_t(0), size_t(1), size_t(17), size_t(19)})
		if (!Complete(memory, 1, identifier, AGE, IDENTIFIER + offset)) return false;
	return Complete(memory, 1, identifier, IDENTIFIER, IDENTIFIER + 17, true) &&
	       Complete(memory, 1, identifier, IDENTIFIER + 19, IDENTIFIER, true) &&
	       Reject(memory, 1, identifier, identifier, 0, Result::InvalidRestrictionsOutput,
	              "invalid restrictions changed an age output aliased with input") &&
	       Reject(memory, 1, identifier, 0, identifier, Result::InvalidAgeOutput,
	              "invalid age changed restrictions aliased with input") &&
	       Check(Network::ResetForNextSession(), "alias-test session reset failed");
}

bool TestConcurrentInvalidation(Mapping& memory)
{
	// Only each worker accesses its own outputs. Mappings and identifier bytes
	// remain stable; this checks request invalidation, not VM transactionality
	// or simultaneous visibility of the two output writes to another observer.
	for (bool use_reset: {false, true})
	{
		if (!Check(CreateRequest() == 1, "concurrent request creation failed")) return false;
		std::atomic<bool> start {false};
		std::atomic<bool> invalidated {false};
		std::atomic<unsigned> entered {0};
		std::atomic<bool> valid {true};
		std::array<std::thread, 4> workers;
		for (size_t worker = 0; worker < workers.size(); ++worker)
		{
			workers[worker] = std::thread([&, worker] {
				uint8_t* output = memory.Data(400 + worker * 32);
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				entered.fetch_add(1, std::memory_order_release);
				for (unsigned iteration = 0; iteration < 256; ++iteration)
				{
					const bool after = invalidated.load(std::memory_order_acquire);
					std::memset(output, 0xa5, 4);
					const auto result = Parental::QueryModel(1, memory.Address(IDENTIFIER), reinterpret_cast<uint64_t>(output),
					                                         reinterpret_cast<uint64_t>(output + 1));
					const bool written = output[0] == 0 && output[1] == 1 && output[2] == 1 && output[3] == 1;
					const bool unchanged = output[0] == 0xa5 && output[1] == 0xa5 && output[2] == 0xa5 && output[3] == 0xa5;
					if ((result != Result::Ok && result != Result::InvalidRequest) || (after && result != Result::InvalidRequest) ||
					    (result == Result::Ok ? !written : !unchanged)) valid.store(false, std::memory_order_relaxed);
				}
				while (!invalidated.load(std::memory_order_acquire)) std::this_thread::yield();
				std::memset(output, 0xb7, 4);
				if (Parental::QueryModel(1, memory.Address(IDENTIFIER), reinterpret_cast<uint64_t>(output),
				                         reinterpret_cast<uint64_t>(output + 1)) != Result::InvalidRequest ||
				    output[0] != 0xb7 || output[1] != 0xb7 || output[2] != 0xb7 || output[3] != 0xb7)
					valid.store(false, std::memory_order_relaxed);
			});
		}
		start.store(true, std::memory_order_release);
		while (entered.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
		bool invalidation_ok = true;
		if (use_reset) Requests::ResetSessionState();
		else invalidation_ok = Np::NpDeleteRequest(1) == 0;
		invalidated.store(true, std::memory_order_release);
		for (auto& worker: workers) worker.join();
		if (!Check(invalidation_ok && valid.load(std::memory_order_relaxed), "request invalidation admitted a later query or changed rejected outputs") ||
		    !Check(Network::ResetForNextSession(), "concurrency session reset failed")) return false;
	}
	return true;
}

bool TestLatch(Mapping& memory, bool plus, bool parental, const char* first)
{
	// Observe only the requested first entrypoint before changing both variables.
	// The other service must already have latched its original value too.
	if (std::strcmp(first, "plus") == 0)
	{
		if (!Check(Plus::ExperimentalEnabled() == plus, "first Plus observation disagreed with the environment")) return false;
	} else if (std::strcmp(first, "parental") == 0)
	{
		if (!Check(Parental::ExperimentalEnabled() == parental, "first parental observation disagreed with the environment")) return false;
	} else if (std::strcmp(first, "create") == 0)
	{
		if (!Check(CreateRequest() == 1, "first request creation failed during latch test")) return false;
	} else if (std::strcmp(first, "delete") == 0)
	{
		if (!Check(Np::NpDeleteRequest(1) == 0, "first legacy deletion failed during latch test")) return false;
	} else return Check(false, "invalid first-access latch scenario");
	if (!Check(setenv(PLUS_SELECTOR, plus ? "0" : "1", 1) == 0 && setenv(PARENTAL_SELECTOR, parental ? "0" : "1", 1) == 0,
	           "could not change both selector variables") || !Selectors(plus, parental) || !TestMatrix(memory, plus, parental)) return false;
	Requests::ResetSessionState();
	return Selectors(plus, parental);
}

int FatalChild(const char* scenario, Mapping& memory)
{
	int request = 1;
	const void* identifier = memory.Data(IDENTIFIER);
	void* age = memory.Data(AGE);
	void* restrictions = memory.Data(RESTRICTIONS);
	if (std::strcmp(scenario, "disabled") != 0 && CreateRequest() != 1) return 3;
	if (std::strcmp(scenario, "request") == 0) request = 2;
	else if (std::strcmp(scenario, "identifier") == 0) identifier = nullptr;
	else if (std::strcmp(scenario, "age") == 0) age = nullptr;
	else if (std::strcmp(scenario, "restrictions") == 0) restrictions = nullptr;
	else if (std::strcmp(scenario, "overlap") == 0) age = memory.Data(RESTRICTIONS + 1);
	else if (std::strcmp(scenario, "disabled") != 0) return 3;
	(void)Parental::GetParentalControlInfo(request, identifier, age, restrictions);
	return 4;
}

bool ExpectFatal(const char* executable, const char* scenario, const char* diagnostic)
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
	// Require Lumen's explicit fatal exit, not a crash or arbitrary error return.
	const bool passed = read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff) &&
	                    captured.find(diagnostic) != std::string::npos;
	if (!passed) std::fprintf(stderr, "NpParentalTest: fatal scenario %s status=%d output=%s\n", scenario, status, captured.c_str());
	return Check(passed, "unsupported wrapper did not stop with its specific diagnostic");
}

bool TestFatalPaths(const char* executable)
{
	return ExpectFatal(executable, "disabled", "result=disabled experimental=1") &&
	       ExpectFatal(executable, "request", "result=request-not-live experimental=1") &&
	       ExpectFatal(executable, "identifier", "result=identifier-unreadable experimental=1") &&
	       ExpectFatal(executable, "age", "result=age-unwritable experimental=1") &&
	       ExpectFatal(executable, "restrictions", "result=restrictions-unwritable experimental=1") &&
	       ExpectFatal(executable, "overlap", "result=outputs-overlap experimental=1");
}

bool BooleanArgument(const char* argument)
{
	return std::strcmp(argument, "0") == 0 || std::strcmp(argument, "1") == 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		const bool disabled = std::strcmp(argv[2], "disabled") == 0;
		if (setenv(PLUS_SELECTOR, "0", 1) != 0 || setenv(PARENTAL_SELECTOR, disabled ? "0" : "1", 1) != 0) return 3;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Vm::Init();
	Network::NetworkSubsystem::Instance()->Init(nullptr);
	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (host_page_size <= 0) return 1;
	const size_t page_size = static_cast<size_t>(host_page_size);
	Mapping memory(page_size);
	if (!Check(memory.Valid() && memory.Size() >= 1024, "main fixture allocation failed")) return 1;
	Fill(memory);
	bool passed = false;
	if (argc == 4 && std::strcmp(argv[1], "matrix") == 0 && BooleanArgument(argv[2]) && BooleanArgument(argv[3]))
	{
		passed = TestMatrix(memory, std::strcmp(argv[2], "1") == 0, std::strcmp(argv[3], "1") == 0);
	} else if (argc == 5 && std::strcmp(argv[1], "latch") == 0 && BooleanArgument(argv[2]) && BooleanArgument(argv[3]))
	{
		passed = TestLatch(memory, std::strcmp(argv[2], "1") == 0, std::strcmp(argv[3], "1") == 0, argv[4]);
	} else if (argc == 2 && std::strcmp(argv[1], "memory") == 0)
	{
		passed = Selectors(false, true) && Registration(false, true) && TestMemory(memory, page_size) &&
		         TestBoundaries(memory, page_size) && TestAliases(memory) && TestConcurrentInvalidation(memory);
	} else if (argc == 2 && std::strcmp(argv[1], "fatal-paths") == 0)
	{
		passed = TestFatalPaths(argv[0]);
	} else if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		return FatalChild(argv[2], memory);
	} else
	{
		std::fprintf(stderr, "Usage: NpParentalTest matrix <plus> <parental>|latch <plus> <parental> plus|parental|create|delete|memory|fatal-paths\n");
		return 2;
	}
	if (!passed) return 1;
	std::puts("NpParentalTest: PASS");
	return 0;
}
