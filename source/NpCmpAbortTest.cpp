#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

namespace Network = Lumen::Libs::Network;
namespace Np = Network::NpManager;
namespace Common = Network::NpCommon;
namespace Requests = Lumen::Libs::NpRequests;
namespace Plus = Lumen::Libs::NpPlus;
namespace Parental = Lumen::Libs::NpParental;
namespace Posix = Lumen::Libs::Posix;
namespace Vm = Lumen::Core::VirtualMemory;

// Original fixtures for NP_CMP_ABORT_INTERFACE.md's chosen local contract.
// NUL handling, record initialization and request quiescence are local policy,
// not a claim of console compatibility or cancellation of a PSN operation.
constexpr uint32_t INVALID_ARGUMENT = 0x80550003u;
constexpr uint32_t NOT_MATCH = 0x80550609u;
constexpr int ERRNO_SENTINEL = 19073;
constexpr size_t NP_ID_BYTES = 36;
constexpr size_t ONLINE_ID_BYTES = 20;
constexpr size_t VM_UNIT = 65536;
using Identifier = std::array<uint8_t, NP_ID_BYTES>;
static_assert(sizeof(int) == sizeof(int32_t));

struct ExportIdentity
{
	const char* nid;
	const char* library;
	const char32_t* standard_name;
	uint64_t address;
};

bool Missing(const Lumen::Loader::SymbolDatabase& symbols, const Lumen::Loader::SymbolResolve& query,
             const char* nid, const char* mismatch)
{
	if (symbols.Find(query) == nullptr) return true;
	std::fprintf(stderr, "NpCmpAbortTest: %s resolved with unsupported %s\n", nid, mismatch);
	return false;
}

bool TestIdentity(const Lumen::Loader::SymbolDatabase& symbols, const ExportIdentity& identity)
{
	Lumen::Loader::SymbolResolve exact {};
	exact.name = identity.nid;
	exact.library = identity.library;
	exact.library_version = 1;
	exact.module = identity.library;
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* symbol = symbols.Find(exact);
	bool passed = symbol != nullptr && symbol->vaddr == identity.address;
	if (!passed)
		std::fprintf(stderr, "NpCmpAbortTest: missing or incorrect export %s in %s v1 / module v1.1\n", identity.nid, identity.library);

	for (int version : {0, 2})
	{
		auto wrong = exact;
		wrong.library_version = version;
		passed = Missing(symbols, wrong, identity.nid, "library version") && passed;
		wrong = exact;
		wrong.module_version_major = version;
		passed = Missing(symbols, wrong, identity.nid, "module major version") && passed;
		wrong = exact;
		wrong.module_version_minor = version;
		passed = Missing(symbols, wrong, identity.nid, "module minor version") && passed;
	}
	for (const char* other : {"libSceNpCommon", "libSceNpManager", "libSceNpManagerForToolkit",
	                         "libSceNet", "unrelated_np_cmp_abort_library"})
	{
		if (std::strcmp(other, identity.library) == 0) continue;
		auto wrong = exact;
		wrong.library = other;
		passed = Missing(symbols, wrong, identity.nid, "library namespace") && passed;
		wrong = exact;
		wrong.module = other;
		passed = Missing(symbols, wrong, identity.nid, "module namespace") && passed;
	}
	for (auto type : {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
	                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
	{
		auto wrong = exact;
		wrong.type = type;
		passed = Missing(symbols, wrong, identity.nid, "symbol type") && passed;
	}
	auto wrong = exact;
	wrong.name = "unknown-np-cmp-abort-nid";
	passed = Missing(symbols, wrong, identity.nid, "NID fallback") && passed;
	wrong.name = identity.standard_name;
	passed = Missing(symbols, wrong, identity.nid, "standard-name fallback") && passed;
	if (symbols.FindByStdName(identity.standard_name) != nullptr)
	{
		std::fprintf(stderr, "NpCmpAbortTest: %s has an unexpected standard-name alias\n", identity.nid);
		passed = false;
	}
	return passed;
}

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpCmpAbortTest: %s\n", message);
	return condition;
}

template <typename Function>
bool Returns(Function function, uint32_t expected, const char* message)
{
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const int result = function();
	if (static_cast<uint32_t>(result) == expected && *Posix::GetErrorAddr() == ERRNO_SENTINEL) return true;
	std::fprintf(stderr, "NpCmpAbortTest: %s returned 0x%08x instead of 0x%08x, errno=%d\n",
	             message, static_cast<unsigned>(result), static_cast<unsigned>(expected), *Posix::GetErrorAddr());
	return false;
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
	bool Protect(size_t offset, size_t size, Vm::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Vm::Protect(Address(offset), size, mode);
	}

private:
	size_t m_size;
	uint64_t m_address;
};

const Np::NpId* Id(const void* bytes) { return reinterpret_cast<const Np::NpId*>(bytes); }

bool Compare(const void* left, const void* right, uint32_t expected)
{
	return Returns([&] { return Common::NpCmpNpId(Id(left), Id(right)); }, expected, "comparison");
}

bool ComparePair(const Identifier& left, const Identifier& right, uint32_t expected)
{
	const auto saved_left = left;
	const auto saved_right = right;
	return Compare(left.data(), right.data(), expected) && Compare(right.data(), left.data(), expected) &&
	       Check(left == saved_left && right == saved_right, "comparison changed an input record");
}

Identifier FullName()
{
	Identifier id {};
	for (size_t index = 0; index < 16; ++index) id[index] = static_cast<uint8_t>('a' + index);
	for (size_t index = 20; index < 28; ++index) id[index] = static_cast<uint8_t>(index * 11);
	return id;
}

bool TestComparison()
{
	const Identifier full = FullName();
	if (!Compare(full.data(), full.data(), 0) || !ComparePair(full, full, 0) || !Compare(nullptr, nullptr, INVALID_ARGUMENT)) return false;
	for (size_t index = 0; index < 16; ++index)
	{
		auto different = full;
		different[index] ^= 0x20; // Same ASCII letter with different case.
		if (!ComparePair(full, different, NOT_MATCH)) return false;
	}
	for (size_t index = 20; index < 28; ++index)
	{
		auto different = full;
		different[index] ^= 0x80;
		if (!ComparePair(full, different, NOT_MATCH)) return false;
	}
	auto ignored = full;
	for (size_t index = 16; index < 20; ++index) ignored[index] = static_cast<uint8_t>(index + 93);
	for (size_t index = 28; index < 36; ++index) ignored[index] = static_cast<uint8_t>(index + 51);
	if (!ComparePair(full, ignored, 0)) return false;
	for (size_t length = 0; length < 16; ++length)
	{
		auto left = full;
		left[length] = 0;
		auto right = left;
		for (size_t index = length + 1; index < 16; ++index) right[index] ^= 0xff;
		if (!ComparePair(left, right, 0) || !ComparePair(left, full, NOT_MATCH)) return false;
		right[27] ^= 1;
		if (!ComparePair(left, right, NOT_MATCH)) return false;
	}
	auto high_bytes = full;
	high_bytes[0] = 0x80;
	if (!ComparePair(high_bytes, high_bytes, 0)) return false;
	auto changed = high_bytes;
	changed[0] = 0xff;
	return ComparePair(high_bytes, changed, NOT_MATCH);
}

bool TestProducers()
{
	for (int user : {1, 9, -1})
	{
		for (bool online : {false, true})
		{
			alignas(8) std::array<uint8_t, 64> storage;
			storage.fill(0xe7);
			auto expected = storage;
			const size_t size = online ? ONLINE_ID_BYTES : NP_ID_BYTES;
			std::memset(expected.data() + 8, 0, size);
			std::memcpy(expected.data() + 8, "Lumen", 5);
			if (!Returns([&] {
				return online ? Np::NpGetOnlineId(user, reinterpret_cast<Np::NpOnlineId*>(storage.data() + 8))
				              : Np::NpGetNpId(user, reinterpret_cast<Np::NpId*>(storage.data() + 8));
			}, 0, "identity producer") || !Check(storage == expected, "producer did not initialize exactly its complete record")) return false;
			if (!online)
			{
				Identifier name {};
				std::memcpy(name.data(), "Lumen", 5);
				if (!Compare(storage.data() + 8, name.data(), 0)) return false;
			}
		}
	}
	return true;
}

bool TestComparisonMemory()
{
	Mapping memory(2 * VM_UNIT);
	if (!Check(memory.Valid(), "comparison mapping allocation failed")) return false;
	std::memset(memory.Data(), 0xad, VM_UNIT);
	const Identifier id = FullName();
	std::memcpy(memory.Data(67), id.data(), id.size());
	std::memcpy(memory.Data(VM_UNIT - NP_ID_BYTES), id.data(), id.size());
	const std::vector<uint8_t> before(memory.Data(), memory.Data(VM_UNIT));
	if (!Check(memory.Protect(VM_UNIT, VM_UNIT, Vm::Mode::NoAccess) && memory.Protect(0, VM_UNIT, Vm::Mode::Read),
	           "comparison input protection failed")) return false;
	if (!Compare(memory.Data(67), memory.Data(VM_UNIT - NP_ID_BYTES), 0) ||
	    !Compare(memory.Data(67), memory.Data(67), 0) ||
	    !Compare(nullptr, memory.Data(VM_UNIT), INVALID_ARGUMENT) ||
	    !Compare(memory.Data(VM_UNIT), nullptr, INVALID_ARGUMENT) ||
	    !Check(std::memcmp(memory.Data(), before.data(), before.size()) == 0, "read-only comparison modified bytes")) return false;
	if (!Check(memory.Protect(0, VM_UNIT, Vm::Mode::ReadWrite), "producer input restoration failed")) return false;
	for (bool online : {false, true})
	{
		std::memset(memory.Data(), 0xba, VM_UNIT);
		const size_t size = online ? ONLINE_ID_BYTES : NP_ID_BYTES;
		uint8_t* output = memory.Data(VM_UNIT - size);
		if (!Returns([&] {
			return online ? Np::NpGetOnlineId(1, reinterpret_cast<Np::NpOnlineId*>(output))
			              : Np::NpGetNpId(1, reinterpret_cast<Np::NpId*>(output));
		}, 0, "producer at protected boundary")) return false;
		Identifier expected {};
		std::memcpy(expected.data(), "Lumen", 5);
		if (!Check(std::memcmp(output, expected.data(), size) == 0 && output[-1] == 0xba,
		           "producer boundary footprint changed")) return false;
	}
	return true;
}

void Store64(uint8_t* bytes, uint64_t value)
{
	for (size_t index = 0; index < 8; ++index) bytes[index] = static_cast<uint8_t>(value >> (index * 8));
}

int CreateRequest()
{
	// Existing original Lumen creation fixture, not a new external declaration.
	alignas(8) std::array<uint8_t, 24> parameter {};
	Store64(parameter.data(), parameter.size());
	Store64(parameter.data() + 8, 1);
	parameter[16] = 64;
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const int id = Np::NpCreateAsyncRequest(reinterpret_cast<const Np::NpCreateAsyncRequestParameter*>(parameter.data()));
	if (!Check(*Posix::GetErrorAddr() == ERRNO_SENTINEL, "creation changed guest errno")) return -1;
	return id;
}

bool Abort(int request, uint32_t expected = 0)
{
	return Returns([&] { return Np::NpAbortRequest(request); }, expected, "abort");
}

bool Live(int request, bool expected)
{
	Requests::LiveRequestGuard guard(request);
	return Check(guard.IsLive() == expected, "experiment-only request admission changed");
}

bool LegacyRequest(int request)
{
	int polled = -123;
	return Returns([&] { return Np::NpPollAsync(request, &polled); }, 0, "legacy poll") && polled == 0 &&
	       Returns([&] { return Np::NpCheckNpAvailability(request, "original-np-cmp-abort-test", nullptr); }, 0, "legacy availability");
}

// Service inputs are stable managed bytes; each worker owns a distinct result
// block. These are the preexisting local deny/restrict profiles, not PSN data.
void FillServices(Mapping& memory)
{
	std::memset(memory.Data(), 0x91, VM_UNIT);
	Store64(memory.Data(64), 56);
}

bool Services(Mapping& memory, size_t output, int request, bool plus, bool parental,
              bool invalidated, bool allow_either = false)
{
	std::array<uint8_t, 16> initial;
	initial.fill(0xe9);
	std::memcpy(memory.Data(output), initial.data(), initial.size());
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const auto plus_result = Plus::CheckModel(request, memory.Address(64), memory.Address(output + 2));
	const bool plus_ok = plus_result == Plus::ModelResult::Ok;
	const bool plus_invalid = plus_result == Plus::ModelResult::InvalidRequest;
	const bool plus_valid = !plus ? plus_result == Plus::ModelResult::Disabled
	                           : allow_either ? plus_ok || plus_invalid : invalidated ? plus_invalid : plus_ok;
	if (!plus_valid || *Posix::GetErrorAddr() != ERRNO_SENTINEL) return Check(false, "Plus compatibility outcome changed");
	auto expected = initial;
	if (plus_ok) expected[2] = 0;
	if (!Check(std::memcmp(memory.Data(output), expected.data(), expected.size()) == 0, "Plus output/canaries changed")) return false;
	const auto parental_result = Parental::QueryModel(request, memory.Address(160), memory.Address(output + 5), memory.Address(output + 8));
	const bool parental_ok = parental_result == Parental::ModelResult::Ok;
	const bool parental_invalid = parental_result == Parental::ModelResult::InvalidRequest;
	const bool parental_valid = !parental ? parental_result == Parental::ModelResult::Disabled
	                                   : allow_either ? parental_ok || parental_invalid : invalidated ? parental_invalid : parental_ok;
	if (!parental_valid || *Posix::GetErrorAddr() != ERRNO_SENTINEL) return Check(false, "parental compatibility outcome changed");
	if (parental_ok)
	{
		expected[5] = 0;
		expected[8] = expected[9] = expected[10] = 1;
	}
	return Check(std::memcmp(memory.Data(output), expected.data(), expected.size()) == 0, "parental output/canaries changed");
}

bool TestRequests(Mapping& memory, bool plus, bool parental)
{
	const bool tracking = plus || parental;
	if (!Check(Network::ResetForNextSession(), "request setup reset failed")) return false;
	for (int request : {std::numeric_limits<int>::min(), -1, 0, 1, 2, std::numeric_limits<int>::max()})
		if (!Abort(request, INVALID_ARGUMENT)) return false;
	if (!Check(CreateRequest() == 1, "first real request was not ID 1") || !Live(1, tracking) ||
	    !Services(memory, 512, 1, plus, parental, false)) return false;
	for (unsigned repeat = 0; repeat < 4; ++repeat)
		if (!Abort(1) || !Live(1, tracking) || !LegacyRequest(1) || !Services(memory, 512, 1, plus, parental, false)) return false;
	if (!Abort(2, INVALID_ARGUMENT) || !Abort(0, INVALID_ARGUMENT) || !Live(1, tracking)) return false;
	int last = 1;
	if (tracking)
	{
		if (!Check(CreateRequest() == 2 && CreateRequest() == 3, "invalid abort changed the request allocator")) return false;
		last = 3;
		for (int request : {2, 3})
			if (!Abort(request) || !Abort(request) || !Live(request, true) || !Services(memory, 512, request, plus, parental, false)) return false;
	}
	if (!Returns([] { return Np::NpDeleteRequest(1); }, 0, "delete") ||
	    !Returns([] { return Np::NpDeleteRequest(1); }, 0, "repeated delete") ||
	    !Abort(1, INVALID_ARGUMENT) || !Live(1, false) || !LegacyRequest(1) ||
	    !Services(memory, 512, 1, plus, parental, true)) return false;
	if (tracking && (!Abort(2) || !Abort(3) || !Services(memory, 512, 3, plus, parental, false))) return false;
	if (!Check(Lumen::Libs::ResetSessionState(), "HLE request reset failed")) return false;
	for (int request = 1; request <= last; ++request)
		if (!Abort(request, INVALID_ARGUMENT) || !Live(request, false) || !LegacyRequest(request)) return false;
	if (tracking)
	{
		if (!Check(CreateRequest() == last + 1, "HLE reset changed the request high-water mark") || !Abort(last + 1)) return false;
		Requests::ResetSessionState();
		if (!Abort(last + 1, INVALID_ARGUMENT) || !Live(last + 1, false) || !LegacyRequest(last + 1)) return false;
	}
	if (!Check(Network::ResetForNextSession(), "Network request reset failed") || !Abort(1, INVALID_ARGUMENT) ||
	    !Check(CreateRequest() == 1, "Network reset did not restart the allocator") || !Abort(1) || !Live(1, tracking)) return false;
	Requests::ResetSessionState();
	return Abort(1, INVALID_ARGUMENT) && Live(1, false) && LegacyRequest(1) &&
	       Check(Network::ResetForNextSession(), "request cleanup reset failed");
}

template <typename Predicate>
bool Await(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate())
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::yield();
	}
	return true;
}

bool TestGuardQuiescence(Mapping& memory, bool plus, bool parental)
{
	if (!Check(CreateRequest() == 1, "guard request creation failed")) return false;
	std::atomic<bool> entered {false};
	std::atomic<bool> finished {false};
	std::atomic<bool> published {false};
	bool returned = false;
	bool saw_publication = false;
	bool entered_in_time = false;
	bool blocked = false;
	std::thread worker;
	{
		Requests::LiveRequestGuard held(1);
		if (!Check(held.IsLive() == (plus || parental), "guard admitted a disabled experiment")) return false;
		worker = std::thread([&] {
			entered.store(true, std::memory_order_release);
			returned = Abort(1);
			saw_publication = published.load(std::memory_order_acquire);
			finished.store(true, std::memory_order_release);
		});
		entered_in_time = Await([&] { return entered.load(std::memory_order_acquire); });
		// The existing guard is already acquired before abort starts. This does
		// not assert FIFO ordering among calls which have not acquired the mutex.
		const auto observation_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
		while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < observation_end)
			std::this_thread::yield();
		blocked = !finished.load(std::memory_order_acquire);
		published.store(true, std::memory_order_release);
	}
	worker.join();
	return Check(entered_in_time && blocked && returned && saw_publication, "abort did not wait for an already-held publication guard") &&
	       Abort(1) && Live(1, plus || parental) && Services(memory, 512, 1, plus, parental, false) &&
	       Check(Network::ResetForNextSession(), "guard cleanup reset failed");
}

bool TestConcurrentQueries(Mapping& memory, bool plus, bool parental)
{
	if (!Check(CreateRequest() == 1, "concurrent query request creation failed")) return false;
	std::atomic<bool> start {false};
	std::atomic<bool> valid {true};
	std::array<std::thread, 4> workers;
	const Identifier identity = FullName();
	for (size_t index = 0; index < workers.size(); ++index)
	{
		workers[index] = std::thread([&, index] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (unsigned repeat = 0; repeat < 128; ++repeat)
			{
				if (!Abort(1) || !Services(memory, 512 + index * 64, 1, plus, parental, false) ||
				    !Compare(identity.data(), identity.data(), 0)) valid.store(false, std::memory_order_relaxed);
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (auto& worker : workers) worker.join();
	return Check(valid.load(std::memory_order_relaxed), "concurrent abort poisoned a reusable request") &&
	       Abort(1) && LegacyRequest(1) && Live(1, plus || parental) &&
	       Check(Network::ResetForNextSession(), "concurrent query cleanup failed");
}

bool TestConcurrentInvalidation(Mapping& memory, bool plus, bool parental)
{
	for (unsigned mode = 0; mode < 3; ++mode)
	{
		if (!Check(CreateRequest() == 1, "invalidation request creation failed")) return false;
		std::atomic<bool> start {false};
		std::atomic<bool> invalidated {false};
		std::atomic<unsigned> entered {0};
		std::atomic<bool> valid {true};
		std::array<std::thread, 4> workers;
		for (size_t index = 0; index < workers.size(); ++index)
		{
			workers[index] = std::thread([&, index] {
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				entered.fetch_add(1, std::memory_order_release);
				for (unsigned repeat = 0; repeat < 128; ++repeat)
				{
					const bool after = invalidated.load(std::memory_order_acquire);
					*Posix::GetErrorAddr() = ERRNO_SENTINEL;
					const uint32_t result = static_cast<uint32_t>(Np::NpAbortRequest(1));
					if ((result != 0 && result != INVALID_ARGUMENT) || (after && result != INVALID_ARGUMENT) ||
					    *Posix::GetErrorAddr() != ERRNO_SENTINEL ||
					    !Services(memory, 512 + index * 64, 1, plus, parental, after, !after))
						valid.store(false, std::memory_order_relaxed);
				}
				while (!invalidated.load(std::memory_order_acquire)) std::this_thread::yield();
				if (!Abort(1, INVALID_ARGUMENT) || !Services(memory, 512 + index * 64, 1, plus, parental, true))
					valid.store(false, std::memory_order_relaxed);
			});
		}
		start.store(true, std::memory_order_release);
		const bool entered_in_time = Await([&] { return entered.load(std::memory_order_acquire) == workers.size(); });
		bool removed = true;
		if (mode == 0) removed = Returns([] { return Np::NpDeleteRequest(1); }, 0, "concurrent delete");
		else if (mode == 1) Requests::ResetSessionState();
		else removed = Network::ResetForNextSession();
		invalidated.store(true, std::memory_order_release);
		for (auto& worker : workers) worker.join();
		// No creation occurs during this race: a new session's numerical ID 1
		// must not be mistaken for the request whose invalidation is being tested.
		if (!Check(entered_in_time && removed && valid.load(std::memory_order_relaxed), "abort/invalidation was not coherent") ||
		    !Abort(1, INVALID_ARGUMENT) || !Live(1, false) || !LegacyRequest(1) ||
		    !Check(Network::ResetForNextSession(), "invalidation cleanup reset failed")) return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 1 && (argc != 3 || (std::strcmp(argv[1], "0") != 0 && std::strcmp(argv[1], "1") != 0) ||
	                                  (std::strcmp(argv[2], "0") != 0 && std::strcmp(argv[2], "1") != 0)))
	{
		std::fputs("Usage: NpCmpAbortTest [<plus:0|1> <parental:0|1>]\n", stderr);
		return 2;
	}
	const bool plus = argc == 3 && std::strcmp(argv[1], "1") == 0;
	const bool parental = argc == 3 && std::strcmp(argv[2], "1") == 0;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Loader::SymbolDatabase symbols;
	if (!Lumen::Libs::Init(U"libNet_1", &symbols))
	{
		std::fputs("NpCmpAbortTest: libNet_1 registration failed\n", stderr);
		return 1;
	}
	const std::array<ExportIdentity, 2> identities = {{
	    {"i8UmXTSq7N4", "libSceNpCommon", U"sceNpCmpNpId", reinterpret_cast<uint64_t>(&Common::NpCmpNpId)},
	    {"OzKvTvg3ZYU", "libSceNpManager", U"sceNpAbortRequest", reinterpret_cast<uint64_t>(&Np::NpAbortRequest)}}};
	bool passed = true;
	for (const auto& identity : identities) passed = TestIdentity(symbols, identity) && passed;
	if (!passed) return 1;
	if (!Check(Plus::ExperimentalEnabled() == plus && Parental::ExperimentalEnabled() == parental &&
	               Requests::TrackingEnabled() == (plus || parental), "independent service selectors changed") ||
	    !Abort(1, INVALID_ARGUMENT) || !TestComparison() || !TestProducers()) return 1;
	Vm::Init();
	if (!TestComparisonMemory()) return 1;
	Network::NetworkSubsystem::Instance()->Init(nullptr);
	Mapping memory(VM_UNIT);
	if (!Check(memory.Valid(), "service mapping allocation failed")) return 1;
	FillServices(memory);
	if (!TestRequests(memory, plus, parental) || !TestGuardQuiescence(memory, plus, parental) ||
	    !TestConcurrentQueries(memory, plus, parental) || !TestConcurrentInvalidation(memory, plus, parental)) return 1;
	std::puts("NpCmpAbortTest: PASS");
	return 0;
}
