#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
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

namespace NpPlus = Lumen::Libs::NpPlus;
namespace NpRequests = Lumen::Libs::NpRequests;
namespace Network = Lumen::Libs::Network;
namespace NpManager = Network::NpManager;
namespace Vm = Lumen::Core::VirtualMemory;
using Result = NpPlus::ModelResult;

constexpr const char* SELECTOR = "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE";
constexpr size_t DESCRIPTOR_BYTES = 56;
constexpr size_t DESCRIPTOR_OFFSET = 64;
constexpr size_t RESULT_OFFSET = 256;
constexpr int EXPERIMENTAL_REQUEST_LIMIT = 65536;

// Original synthetic fixtures test the approved local experiment, not console
// behavior or entitlement. No external source or external test is used here.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpPlusTest: %s\n", message);
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
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }
	size_t Size() const { return m_size; }
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

void StoreLittleEndian64(uint8_t* destination, uint64_t value)
{
	for (size_t byte = 0; byte < 8; ++byte) destination[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

void PutDescriptor(uint8_t* destination, uint8_t seed = 19)
{
	for (size_t byte = 0; byte < DESCRIPTOR_BYTES; ++byte)
		destination[byte] = static_cast<uint8_t>(seed + byte * 43);
	StoreLittleEndian64(destination, DESCRIPTOR_BYTES);
}

void Fill(Mapping& memory)
{
	for (size_t byte = 0; byte < memory.Size(); ++byte)
		memory.Data()[byte] = static_cast<uint8_t>(byte * 29 + 71);
	PutDescriptor(memory.Data(DESCRIPTOR_OFFSET));
}

std::vector<uint8_t> Snapshot(const Mapping& memory)
{
	return {memory.Data(), memory.Data() + memory.Size()};
}

bool Matches(const Mapping& memory, const std::vector<uint8_t>& expected)
{
	return std::memcmp(memory.Data(), expected.data(), expected.size()) == 0;
}

bool RejectedWithoutWrite(const Mapping& memory, int32_t request, uint64_t descriptor, uint64_t output,
                          Result expected, const char* message)
{
	const auto before = Snapshot(memory);
	return Check(NpPlus::CheckModel(request, descriptor, output) == expected, message) &&
	       Check(Matches(memory, before), "rejected query changed descriptor, output, reserved bytes or guards");
}

int CreateRequest()
{
	// The public parameter is incomplete. This original byte fixture follows
	// only the existing in-tree size/cpumask/priority/padding fields (24 bytes).
	alignas(8) std::array<uint8_t, 24> parameter {};
	StoreLittleEndian64(parameter.data(), parameter.size());
	StoreLittleEndian64(parameter.data() + 8, 1);
	parameter[16] = 64;
	return NpManager::NpCreateAsyncRequest(
	    reinterpret_cast<const NpManager::NpCreateAsyncRequestParameter*>(parameter.data()));
}

bool TestRegistration(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libNet_1", &symbols), "existing Net library selection failed")) return false;
	struct Export { const char* nid; uint64_t address; };
	const std::array<Export, 14> existing {{
	    {"3Zl8BePTh9Y", reinterpret_cast<uint64_t>(&NpManager::NpCheckCallback)},
	    {"Ec63y59l9tw", reinterpret_cast<uint64_t>(&NpManager::NpSetNpTitleId)},
	    {"A2CQ3kgSopQ", reinterpret_cast<uint64_t>(&NpManager::NpSetContentRestriction)},
	    {"VfRSmPmj8Q8", reinterpret_cast<uint64_t>(&NpManager::NpRegisterStateCallback)},
	    {"uFJpaKNBAj4", reinterpret_cast<uint64_t>(&NpManager::NpRegisterGamePresenceCallback)},
	    {"GImICnh+boA", reinterpret_cast<uint64_t>(&NpManager::NpRegisterPlusEventCallback)},
	    {"hw5KNqAAels", reinterpret_cast<uint64_t>(&NpManager::NpRegisterNpReachabilityStateCallback)},
	    {"p-o74CnoNzY", reinterpret_cast<uint64_t>(&NpManager::NpGetNpId)},
	    {"XDncXQIJUSk", reinterpret_cast<uint64_t>(&NpManager::NpGetOnlineId)},
	    {"eiqMCt9UshI", reinterpret_cast<uint64_t>(&NpManager::NpCreateAsyncRequest)},
	    {"S7QTn72PrDw", reinterpret_cast<uint64_t>(&NpManager::NpDeleteRequest)},
	    {"2rsFmlGWleQ", reinterpret_cast<uint64_t>(&NpManager::NpCheckNpAvailability)},
	    {"uqcPJLWL08M", reinterpret_cast<uint64_t>(&NpManager::NpPollAsync)},
	    {"eQH7nWPcAgc", reinterpret_cast<uint64_t>(&NpManager::NpGetState)},
	}};
	Lumen::Loader::SymbolResolve query {};
	query.library = "libSceNpManager";
	query.library_version = 1;
	query.module = "libSceNpManager";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	for (const auto& expected: existing)
	{
		query.name = expected.nid;
		const auto* record = symbols.Find(query);
		if (!Check(record != nullptr && record->vaddr == expected.address, "an existing NP binding changed")) return false;
	}
	query.name = "r6MyYJkryz8";
	const auto* plus = symbols.Find(query);
	if (!Check(enabled ? plus != nullptr && plus->vaddr == reinterpret_cast<uint64_t>(&NpPlus::CheckPlus) : plus == nullptr,
	           "Plus export selection or target does not match the selector")) return false;
	const auto exact = query;
	for (int version: {0, 2})
	{
		query = exact;
		query.library_version = version;
		if (!Check(symbols.Find(query) == nullptr, "Plus resolved in an unsupported library version")) return false;
		query = exact;
		query.module_version_major = version;
		if (!Check(symbols.Find(query) == nullptr, "Plus resolved in an unsupported module major version")) return false;
		query = exact;
		query.module_version_minor = version;
		if (!Check(symbols.Find(query) == nullptr, "Plus resolved in an unsupported module minor version")) return false;
	}
	for (const char* library: {"libSceNpManagerForToolkit", "libSceNet", "libSceUnknownNpPlusTest"})
	{
		query = exact;
		query.library = library;
		if (!Check(symbols.Find(query) == nullptr, "Plus resolved in an unrelated library")) return false;
		query = exact;
		query.module = library;
		if (!Check(symbols.Find(query) == nullptr, "Plus resolved in an unrelated module")) return false;
	}
	query = exact;
	query.type = Lumen::Loader::SymbolType::Object;
	if (!Check(symbols.Find(query) == nullptr, "Plus resolved as a data object")) return false;
	for (const char* name: {"unknown-np-plus-test-nid", "sceNpCheckPlus"})
	{
		query = exact;
		query.name = name;
		if (!Check(symbols.Find(query) == nullptr, "unknown or standard-name fallback resolved")) return false;
	}
	Lumen::Loader::SymbolDatabase remoteplay;
	return Check(symbols.FindByStdName(U"sceNpCheckPlus") == nullptr && symbols.StdNameMapSize() == 0,
	             "Plus registered a standard-name alias") &&
	       Check(!Lumen::Libs::Init(U"libRemoteplay_1", &remoteplay), "Plus enabled the independent Remoteplay library");
}

bool TestLegacyAndLifecycle(Mapping& memory, bool enabled)
{
	const uint64_t descriptor = memory.Address(DESCRIPTOR_OFFSET);
	const uint64_t output = memory.Address(RESULT_OFFSET);
	const auto inactive = enabled ? Result::InvalidRequest : Result::Disabled;
	if (!Check(Network::ResetForNextSession(), "Network reset failed") ||
	    !RejectedWithoutWrite(memory, 1, descriptor, output, inactive, "request 1 was inferred live before creation")) return false;
	const int request = CreateRequest();
	uint32_t state = 0xfeedbeef;
	int polled = 0x7fffffff;
	if (!Check(request == 1, "legacy NP first request identifier changed") ||
	    !Check(NpManager::NpGetState(9, &state) == 0 && state == 0, "existing NP state result changed") ||
	    !Check(NpManager::NpPollAsync(request, &polled) == 0 && polled == 0, "existing NP poll result changed") ||
	    !Check(NpManager::NpCheckNpAvailability(request, "original-np-plus-test", nullptr) == 0,
	           "existing NP availability behavior changed")) return false;
	if (enabled)
	{
		for (unsigned repetition = 0; repetition < 3; ++repetition)
		{
			memory.Data()[RESULT_OFFSET] = 0xa5;
			auto expected = Snapshot(memory);
			expected[RESULT_OFFSET] = 0;
			if (!Check(NpPlus::CheckPlus(request, memory.Data(DESCRIPTOR_OFFSET), memory.Data(RESULT_OFFSET)) == 0,
			           "supported wrapper query did not complete") ||
			    !Check(Matches(memory, expected), "query wrote beyond its single denied byte")) return false;
		}
	} else if (!RejectedWithoutWrite(memory, request, descriptor, output, Result::Disabled, "disabled live query was admitted"))
		return false;
	if (!Check(NpManager::NpDeleteRequest(request) == 0 && NpManager::NpDeleteRequest(request) == 0,
	           "existing repeated deletion result changed") ||
	    !RejectedWithoutWrite(memory, request, descriptor, output, inactive, "deleted request remained live")) return false;
	polled = 123;
	if (!Check(NpManager::NpPollAsync(request, &polled) == 0 && polled == 0,
	           "Plus tracking changed legacy poll-after-delete behavior") ||
	    !Check(Network::ResetForNextSession(), "Network lifecycle reset failed") ||
	    !Check(CreateRequest() == 1, "Network reset did not restore the existing request allocator")) return false;
	if (enabled && !Check(NpPlus::CheckModel(1, descriptor, output) == Result::Ok, "request after Network reset was not recorded"))
		return false;
	if (!Check(Network::ResetForNextSession(), "Network reset with a live request failed") ||
	    !RejectedWithoutWrite(memory, 1, descriptor, output, inactive, "Network reset retained live Plus state") ||
	    !Check(CreateRequest() == 1, "second Network session did not start with legacy request 1") ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed") ||
	    !RejectedWithoutWrite(memory, 1, descriptor, output, inactive, "aggregate HLE reset retained live Plus state")) return false;
	return Check(Network::ResetForNextSession(), "final Network lifecycle reset failed");
}

bool DeniedWithCanaries(Mapping& memory, int request)
{
	memory.Data()[RESULT_OFFSET] = 0xcb;
	auto expected = Snapshot(memory);
	expected[RESULT_OFFSET] = 0;
	return Check(NpPlus::CheckPlus(request, memory.Data(DESCRIPTOR_OFFSET), memory.Data(RESULT_OFFSET)) == 0,
	             "issued request did not complete its local Plus query") &&
	       Check(Matches(memory, expected), "issued request changed bytes outside its denial result");
}

bool TestMultipleIssuedRequests(Mapping& memory)
{
	const uint64_t descriptor = memory.Address(DESCRIPTOR_OFFSET);
	const uint64_t output = memory.Address(RESULT_OFFSET);
	if (!Check(Network::ResetForNextSession(), "multiple-request setup reset failed") ||
	    !Check(CreateRequest() == 1, "multiple-request session did not begin with ID 1") ||
	    !RejectedWithoutWrite(memory, 2, descriptor, output, Result::InvalidRequest, "future request 2 was inferred live") ||
	    !Check(CreateRequest() == 2 && CreateRequest() == 3, "enabled allocator did not issue distinct IDs 2 and 3")) return false;
	for (int request: {1, 2, 3})
	{
		int polled = -1;
		if (!DeniedWithCanaries(memory, request) ||
		    !Check(NpManager::NpPollAsync(request, &polled) == 0 && polled == 0, "issued request poll failed") ||
		    !Check(NpManager::NpCheckNpAvailability(request, "original-multiple-request-test", nullptr) == 0,
		           "issued request availability failed")) return false;
	}
	int polled = -1;
	if (!Check(NpManager::NpDeleteRequest(2) == 0 && NpManager::NpDeleteRequest(2) == 0,
	           "repeated deletion of issued request 2 failed") ||
	    !RejectedWithoutWrite(memory, 2, descriptor, output, Result::InvalidRequest, "deleted request 2 remained live for Plus") ||
	    !DeniedWithCanaries(memory, 1) || !DeniedWithCanaries(memory, 3) ||
	    !Check(NpManager::NpPollAsync(2, &polled) == 0 && polled == 0, "issued deleted request lost existing poll behavior") ||
	    !Check(NpManager::NpCheckNpAvailability(2, "original-multiple-request-test", nullptr) == 0,
	           "issued deleted request lost existing availability behavior") ||
	    !RejectedWithoutWrite(memory, 4, descriptor, output, Result::InvalidRequest, "future request 4 was inferred live") ||
	    !Check(CreateRequest() == 4, "deletion recycled an identifier instead of advancing the enabled allocator") ||
	    !DeniedWithCanaries(memory, 4) ||
	    !Check(Lumen::Libs::ResetSessionState(), "multiple-request aggregate reset failed")) return false;
	for (int request: {1, 2, 3, 4})
		if (!RejectedWithoutWrite(memory, request, descriptor, output, Result::InvalidRequest,
		                          "aggregate reset retained a live request from the multi-request session")) return false;
	if (!Check(CreateRequest() == 5, "aggregate HLE reset unexpectedly changed the Network allocator") ||
	    !DeniedWithCanaries(memory, 5) ||
	    !Check(Network::ResetForNextSession(), "multiple-request Network reset failed")) return false;
	for (int request: {1, 2, 3, 4, 5})
		if (!RejectedWithoutWrite(memory, request, descriptor, output, Result::InvalidRequest,
		                          "Network reset retained a request from the multi-request session")) return false;
	return Check(CreateRequest() == 1, "Network reset did not restart the multiple-request allocator at 1") &&
	       DeniedWithCanaries(memory, 1) &&
	       RejectedWithoutWrite(memory, 2, descriptor, output, Result::InvalidRequest, "new session inherited old request 2") &&
	       Check(Network::ResetForNextSession(), "multiple-request final reset failed");
}

bool TestCheckedMemory(Mapping& memory, size_t page_size)
{
	if (!Check(CreateRequest() == 1, "memory-test request creation failed")) return false;
	const int request = 1;
	const uint64_t descriptor = memory.Address(DESCRIPTOR_OFFSET);
	const uint64_t output = memory.Address(RESULT_OFFSET);
	for (int32_t invalid: {std::numeric_limits<int32_t>::min(), -1, 0, 2, std::numeric_limits<int32_t>::max()})
		if (!RejectedWithoutWrite(memory, invalid, descriptor, output, Result::InvalidRequest, "non-live request was admitted")) return false;
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "unmapped fixture allocation failed")) return false;
	const uint64_t unmapped = freed.Address();
	if (!Check(freed.Free(), "unmapped fixture release failed")) return false;
	constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
	for (uint64_t address: {uint64_t(0), uint64_t(1), unmapped, maximum, maximum - DESCRIPTOR_BYTES + 2})
		if (!RejectedWithoutWrite(memory, request, address, output, Result::InvalidDescriptor, "invalid descriptor range was admitted"))
			return false;
	for (uint64_t address: {uint64_t(0), uint64_t(1), unmapped, maximum})
		if (!RejectedWithoutWrite(memory, request, descriptor, address, Result::InvalidOutput, "invalid output range was admitted"))
			return false;
	if (!RejectedWithoutWrite(memory, request, 0, 0, Result::InvalidDescriptor, "two null pointers were admitted")) return false;
	for (uint64_t size: {uint64_t(0), uint64_t(1), uint64_t(55), uint64_t(57), uint64_t(0x3800000000000000), maximum})
	{
		StoreLittleEndian64(memory.Data(DESCRIPTOR_OFFSET), size);
		if (!RejectedWithoutWrite(memory, request, descriptor, output, Result::UnsupportedFormat, "unsupported descriptor size was admitted"))
			return false;
	}
	PutDescriptor(memory.Data(DESCRIPTOR_OFFSET));
	for (uint8_t seed: {uint8_t(0), uint8_t(0xff), uint8_t(91)})
	{
		PutDescriptor(memory.Data(DESCRIPTOR_OFFSET), seed);
		memory.Data()[RESULT_OFFSET] = 0xee;
		auto expected = Snapshot(memory);
		expected[RESULT_OFFSET] = 0;
		if (!Check(NpPlus::CheckModel(request, descriptor, output) == Result::Ok && Matches(memory, expected),
		           "opaque descriptor data affected denial or changed surrounding bytes")) return false;
	}
	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "protected fixture allocation failed")) return false;
	Fill(protected_memory);
	const auto protected_before = Snapshot(protected_memory);
	if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::Read), "read-only fixture protection failed") ||
	    !RejectedWithoutWrite(memory, request, descriptor, protected_memory.Address(), Result::InvalidOutput,
	                          "read-only output was admitted") ||
	    !Check(Matches(protected_memory, protected_before), "read-only output changed") ||
	    !Check(NpPlus::CheckModel(request, protected_memory.Address(DESCRIPTOR_OFFSET), output) == Result::Ok,
	           "read-only descriptor was rejected") ||
	    !Check(protected_memory.Protect(0, page_size, Vm::Mode::NoAccess), "inaccessible fixture protection failed") ||
	    !RejectedWithoutWrite(memory, request, protected_memory.Address(DESCRIPTOR_OFFSET), output, Result::InvalidDescriptor,
	                          "inaccessible descriptor was admitted") ||
	    !RejectedWithoutWrite(memory, request, descriptor, protected_memory.Address(), Result::InvalidOutput,
	                          "inaccessible output was admitted") ||
	    !Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "fixture protection restore failed") ||
	    !Check(Matches(protected_memory, protected_before), "failed protected-memory query changed its storage")) return false;
	return true;
}

bool TestBoundariesAndOverlap(Mapping& memory, size_t page_size)
{
	const uint64_t descriptor = memory.Address(DESCRIPTOR_OFFSET);
	const uint64_t output = memory.Address(RESULT_OFFSET);
	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "boundary fixture allocation failed")) return false;
	Fill(boundary);
	for (size_t readable: {size_t(1), size_t(8), size_t(32), size_t(55)})
	{
		PutDescriptor(boundary.Data(page_size - readable));
		const auto before = Snapshot(boundary);
		if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "descriptor boundary protection failed") ||
		    !RejectedWithoutWrite(memory, 1, boundary.Address(page_size - readable), output, Result::InvalidDescriptor,
		                          "truncated descriptor was admitted") ||
		    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "descriptor boundary restore failed") ||
		    !Check(Matches(boundary, before), "rejected truncated descriptor was modified")) return false;
	}
	PutDescriptor(boundary.Data(page_size - DESCRIPTOR_BYTES));
	auto boundary_expected = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "exact descriptor boundary protection failed") ||
	    !Check(NpPlus::CheckModel(1, boundary.Address(page_size - DESCRIPTOR_BYTES), output) == Result::Ok,
	           "fully readable descriptor ending at a protected page was rejected")) return false;
	boundary.Data()[page_size - 1] = 0xad;
	boundary_expected[page_size - 1] = 0;
	std::vector<uint8_t> expected(boundary.Data(), boundary.Data() + page_size);
	expected[page_size - 1] = 0;
	if (!Check(NpPlus::CheckModel(1, descriptor, boundary.Address(page_size - 1)) == Result::Ok,
	           "single writable output byte before a protected page was rejected") ||
	    !Check(std::memcmp(boundary.Data(), expected.data(), page_size) == 0, "boundary output wrote beyond one byte") ||
	    !RejectedWithoutWrite(memory, 1, descriptor, boundary.Address(page_size), Result::InvalidOutput,
	                          "protected-page output was admitted") ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "output boundary restore failed") ||
	    !Check(Matches(boundary, boundary_expected), "boundary calls changed protected bytes or other guards")) return false;
	for (size_t overlap: {size_t(0), size_t(7), size_t(8), size_t(16), size_t(55)})
	{
		PutDescriptor(memory.Data(DESCRIPTOR_OFFSET));
		auto original = Snapshot(memory);
		original[DESCRIPTOR_OFFSET + overlap] = 0;
		if (!Check(NpPlus::CheckModel(1, descriptor, descriptor + overlap) == Result::Ok,
		           "descriptor/output overlap was rejected or validation used modified input") ||
		    !Check(Matches(memory, original), "overlap changed bytes outside the result footprint")) return false;
		if (overlap == 0 && !RejectedWithoutWrite(memory, 1, descriptor, output, Result::UnsupportedFormat,
		                                        "later query did not observe the size byte changed by overlap")) return false;
	}
	PutDescriptor(memory.Data(DESCRIPTOR_OFFSET));
	return Check(Network::ResetForNextSession(), "boundary-test session reset failed");
}

bool TestConcurrentDeletion(Mapping& memory)
{
	if (!Check(CreateRequest() == 1, "concurrency request creation failed")) return false;
	std::atomic<bool> start {false};
	std::atomic<bool> deleted {false};
	std::atomic<unsigned> entered {0};
	std::atomic<bool> valid {true};
	std::array<std::thread, 4> workers;
	for (size_t worker = 0; worker < workers.size(); ++worker)
	{
		workers[worker] = std::thread([&, worker] {
			uint8_t* output = memory.Data(RESULT_OFFSET + worker * 64);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			entered.fetch_add(1, std::memory_order_release);
			for (unsigned iteration = 0; iteration < 512; ++iteration)
			{
				const bool after_delete = deleted.load(std::memory_order_acquire);
				*output = 0xa9;
				const auto result = NpPlus::CheckModel(1, memory.Address(DESCRIPTOR_OFFSET), reinterpret_cast<uint64_t>(output));
				if ((result != Result::Ok && result != Result::InvalidRequest) ||
				    (after_delete && result != Result::InvalidRequest) ||
				    *output != (result == Result::Ok ? 0 : 0xa9)) valid.store(false, std::memory_order_relaxed);
			}
			while (!deleted.load(std::memory_order_acquire)) std::this_thread::yield();
			*output = 0xb7;
			if (NpPlus::CheckModel(1, memory.Address(DESCRIPTOR_OFFSET), reinterpret_cast<uint64_t>(output)) != Result::InvalidRequest ||
			    *output != 0xb7) valid.store(false, std::memory_order_relaxed);
		});
	}
	start.store(true, std::memory_order_release);
	while (entered.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
	const bool delete_ok = NpManager::NpDeleteRequest(1) == 0;
	deleted.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();
	return Check(delete_ok && valid.load(std::memory_order_relaxed), "concurrent deletion admitted a later query or exposed a partial result") &&
	       Check(Network::ResetForNextSession(), "concurrency session reset failed");
}

bool TestLatch(bool expected, Mapping& memory)
{
	if (!Check(NpPlus::ExperimentalEnabled() == expected, "initial process selector did not match expectation") ||
	    !Check(setenv(SELECTOR, expected ? "0" : "1", 1) == 0, "could not change latch-test environment") ||
	    !Check(Network::ResetForNextSession() && Lumen::Libs::ResetSessionState(), "latch-test reset failed") ||
	    !Check(NpPlus::ExperimentalEnabled() == expected, "environment change or reset altered the process latch") ||
	    !TestRegistration(expected) || !Check(CreateRequest() == 1, "latch-test request creation failed")) return false;
	const auto result = NpPlus::CheckModel(1, memory.Address(DESCRIPTOR_OFFSET), memory.Address(RESULT_OFFSET));
	NpRequests::ResetSessionState();
	return Check(result == (expected ? Result::Ok : Result::Disabled), "query ignored the process latch") &&
	       Check(NpPlus::ExperimentalEnabled() == expected, "direct model reset changed the process latch") &&
	       RejectedWithoutWrite(memory, 1, memory.Address(DESCRIPTOR_OFFSET), memory.Address(RESULT_OFFSET),
	                            expected ? Result::InvalidRequest : Result::Disabled, "direct model reset retained live state");
}

int RunFatalChild(const char* scenario, Mapping& memory)
{
	if (std::strcmp(scenario, "legacy-disabled") == 0)
	{
		if (CreateRequest() != 1 || NpManager::NpDeleteRequest(1) != 0) return 3;
		std::fprintf(stderr, "NpPlusTest: default first request deleted\n");
		(void)CreateRequest();
		return 4; // Existing allocator must still stop rather than recycle after deletion.
	}
	if (std::strcmp(scenario, "saturation-enabled") == 0)
	{
		// Bound the identifier space twice to verify both its last valid ID and
		// reset from saturation. Suppress only ordinary allocation stdout.
		const int saved_stdout = dup(STDOUT_FILENO);
		FILE* sink = std::fopen("/dev/null", "w");
		if (saved_stdout < 0 || sink == nullptr)
		{
			if (saved_stdout >= 0) close(saved_stdout);
			if (sink != nullptr) std::fclose(sink);
			return 3;
		}
		std::fflush(stdout);
		const int redirected = dup2(fileno(sink), STDOUT_FILENO);
		std::fclose(sink);
		if (redirected < 0) { close(saved_stdout); return 3; }
		const bool saturated = [&] {
			for (int cycle = 0; cycle < 2; ++cycle)
			{
				for (int expected = 1; expected <= EXPERIMENTAL_REQUEST_LIMIT; ++expected)
					if (!Check(CreateRequest() == expected, "enabled allocation skipped, reused or exceeded a bounded ID")) return false;
				int polled = -1;
				if (!DeniedWithCanaries(memory, EXPERIMENTAL_REQUEST_LIMIT) ||
				    !Check(NpManager::NpPollAsync(EXPERIMENTAL_REQUEST_LIMIT, &polled) == 0 && polled == 0,
				           "last issued request could not be polled") ||
				    !Check(NpManager::NpCheckNpAvailability(EXPERIMENTAL_REQUEST_LIMIT, "original-cap-test", nullptr) == 0,
				           "last issued request availability failed") ||
				    !RejectedWithoutWrite(memory, EXPERIMENTAL_REQUEST_LIMIT + 1, memory.Address(DESCRIPTOR_OFFSET),
				                          memory.Address(RESULT_OFFSET), Result::InvalidRequest, "above-limit request was admitted")) return false;
				if (cycle == 0 &&
				    (!Check(Network::ResetForNextSession(), "saturated Network reset failed") ||
				     !RejectedWithoutWrite(memory, EXPERIMENTAL_REQUEST_LIMIT, memory.Address(DESCRIPTOR_OFFSET),
				                           memory.Address(RESULT_OFFSET), Result::InvalidRequest, "reset retained the last saturated request")))
					return false;
			}
			int polled = -1;
			return Check(NpManager::NpDeleteRequest(EXPERIMENTAL_REQUEST_LIMIT) == 0 &&
			                 NpManager::NpDeleteRequest(EXPERIMENTAL_REQUEST_LIMIT) == 0,
			             "last issued request could not be repeatedly deleted") &&
			       RejectedWithoutWrite(memory, EXPERIMENTAL_REQUEST_LIMIT, memory.Address(DESCRIPTOR_OFFSET),
			                            memory.Address(RESULT_OFFSET), Result::InvalidRequest, "deleted last request remained live") &&
			       Check(NpManager::NpPollAsync(EXPERIMENTAL_REQUEST_LIMIT, &polled) == 0 && polled == 0,
			             "last deleted request lost existing poll behavior");
		}();
		std::fflush(stdout);
		const int restored = dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
		if (!saturated || restored < 0) return 3;
		std::fprintf(stderr, "NpPlusTest: saturated reset and last ID validated\n");
		(void)CreateRequest();
		return 4; // Saturation must not wrap or recycle a deleted identifier.
	}
	if (std::strcmp(scenario, "unknown-poll") == 0)
	{
		int polled = -1;
		(void)NpManager::NpPollAsync(2, &polled);
		return 4;
	}
	if (std::strcmp(scenario, "unknown-delete") == 0)
	{
		(void)NpManager::NpDeleteRequest(2);
		return 4;
	}
	if (std::strcmp(scenario, "unknown-availability") == 0)
	{
		(void)NpManager::NpCheckNpAvailability(2, "original-unissued-request-test", nullptr);
		return 4;
	}
	int request = 1;
	const void* descriptor = memory.Data(DESCRIPTOR_OFFSET);
	void* output = memory.Data(RESULT_OFFSET);
	if (std::strcmp(scenario, "disabled") != 0 && CreateRequest() != 1) return 3;
	if (std::strcmp(scenario, "request") == 0) request = 2;
	else if (std::strcmp(scenario, "descriptor") == 0) descriptor = nullptr;
	else if (std::strcmp(scenario, "format") == 0) StoreLittleEndian64(memory.Data(DESCRIPTOR_OFFSET), 55);
	else if (std::strcmp(scenario, "output") == 0) output = nullptr;
	else if (std::strcmp(scenario, "disabled") != 0) return 3;
	(void)NpPlus::CheckPlus(request, descriptor, output);
	return 4;
}

bool ExpectFatalChild(const char* executable, const char* scenario, const char* diagnostic, const char* setup_marker = nullptr)
{
	int descriptors[2] {};
	if (!Check(pipe(descriptors) == 0, "fatal-child output pipe failed")) return false;
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
	// 321 is the existing explicit fatal-exit code; POSIX preserves its low byte.
	// A crash, ordinary error return or arbitrary nonzero exit must not pass.
	const bool passed = read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff) &&
	                    captured.find(diagnostic) != std::string::npos &&
	                    (setup_marker == nullptr || captured.find(setup_marker) != std::string::npos);
	if (!passed) std::fprintf(stderr, "NpPlusTest: fatal scenario %s status=%d output=%s\n", scenario, status, captured.c_str());
	return Check(passed, "unsupported wrapper did not stop with the expected specific diagnostic");
}

bool TestFatalPaths(const char* executable)
{
	return ExpectFatalChild(executable, "disabled", "result=disabled experimental=1") &&
	       ExpectFatalChild(executable, "request", "result=request-not-live experimental=1") &&
	       ExpectFatalChild(executable, "descriptor", "result=descriptor-unreadable experimental=1") &&
	       ExpectFatalChild(executable, "format", "result=descriptor-format-unsupported experimental=1") &&
	       ExpectFatalChild(executable, "output", "result=result-unwritable experimental=1") &&
	       ExpectFatalChild(executable, "legacy-disabled", "Not implemented (id < 0)", "NpPlusTest: default first request deleted") &&
	       ExpectFatalChild(executable, "saturation-enabled", "Not implemented (id < 0)", "NpPlusTest: saturated reset and last ID validated") &&
	       ExpectFatalChild(executable, "unknown-poll", "Not implemented (!IsSupportedRequestId(req_id))") &&
	       ExpectFatalChild(executable, "unknown-delete", "Not implemented (!IsSupportedRequestId(req_id))") &&
	       ExpectFatalChild(executable, "unknown-availability", "Not implemented (!IsSupportedRequestId(req_id))");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		const bool disabled = std::strcmp(argv[2], "disabled") == 0 || std::strcmp(argv[2], "legacy-disabled") == 0;
		if (setenv(SELECTOR, disabled ? "0" : "1", 1) != 0) return 3;
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
	if (!Check(memory.Valid() && memory.Size() >= 512, "main fixture allocation failed")) return 1;
	Fill(memory);
	bool passed = false;
	if (argc == 2 && std::strcmp(argv[1], "disabled") == 0)
	{
		passed = Check(!NpPlus::ExperimentalEnabled(), "disabled mode enabled Plus") && TestRegistration(false) &&
		         RejectedWithoutWrite(memory, 1, 0, 0, Result::Disabled, "disabled model did not reject before memory access") &&
		         TestLegacyAndLifecycle(memory, false);
	} else if (argc == 2 && std::strcmp(argv[1], "enabled") == 0)
	{
		passed = Check(NpPlus::ExperimentalEnabled(), "enabled mode did not enable Plus") && TestRegistration(true) &&
		         TestLegacyAndLifecycle(memory, true) && TestMultipleIssuedRequests(memory) && TestCheckedMemory(memory, page_size) &&
		         TestBoundariesAndOverlap(memory, page_size) && TestConcurrentDeletion(memory);
	} else if (argc == 3 && std::strcmp(argv[1], "latch") == 0 &&
	           (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
	{
		passed = TestLatch(std::strcmp(argv[2], "1") == 0, memory);
	} else if (argc == 2 && std::strcmp(argv[1], "fatal-paths") == 0)
	{
		passed = TestFatalPaths(argv[0]);
	} else if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		return RunFatalChild(argv[2], memory);
	} else
	{
		std::fprintf(stderr, "Usage: NpPlusTest disabled|enabled|latch 0|1|fatal-paths\n");
		return 2;
	}
	if (!passed) return 1;
	std::puts("NpPlusTest: PASS");
	return 0;
}
