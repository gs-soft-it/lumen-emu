#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

namespace Net = Lumen::Libs::Network::Net;
namespace Vm = Lumen::Core::VirtualMemory;
using Lumen::Loader::SymbolDatabase;
using Lumen::Loader::SymbolResolve;
using Lumen::Loader::SymbolType;

using ErrorLocation = int* (LUMEN_SYSV_ABI*)();
using NoArguments = int (LUMEN_SYSV_ABI*)();
using PoolCreate = int (LUMEN_SYSV_ABI*)(const char*, int, int);
using OneInt = int (LUMEN_SYSV_ABI*)(int);
using Accept = int (LUMEN_SYSV_ABI*)(int, void*, uint32_t*);
using AddressOperation = int (LUMEN_SYSV_ABI*)(int, const void*, uint32_t);
using EpollControl = int (LUMEN_SYSV_ABI*)(int, int, int, void*);
using EpollCreate = int (LUMEN_SYSV_ABI*)(const char*, int);
using EpollWait = int (LUMEN_SYSV_ABI*)(int, void*, int, int);
// This arity qualifies only Lumen's fail-closed bridge model.
using GetSockInfo = int (LUMEN_SYSV_ABI*)(int, void*, int, int);
using Getsockname = int (LUMEN_SYSV_ABI*)(int, void*, uint32_t*);
using Getsockopt = int (LUMEN_SYSV_ABI*)(int, int, int, void*, uint32_t*);
using Htons = uint16_t (LUMEN_SYSV_ABI*)(uint16_t);
using InetPton = int (LUMEN_SYSV_ABI*)(int, const char*, void*);
using Listen = int (LUMEN_SYSV_ABI*)(int, int);
using Receive = int (LUMEN_SYSV_ABI*)(int, void*, uint64_t, int);
using ReceiveFrom = int (LUMEN_SYSV_ABI*)(int, void*, uint64_t, int, void*, uint32_t*);
using ResolverCreate = int (LUMEN_SYSV_ABI*)(const char*, int, int);
using ResolverGetError = int (LUMEN_SYSV_ABI*)(int, int32_t*);
// These arities qualify only Lumen's fail-closed bridge model. Resolver
// behavior and complete console ABI equivalence remain unverified.
using ResolverStartAton = int (LUMEN_SYSV_ABI*)(int, const void*, char*, int, int, int, int);
using ResolverStartNtoa = int (LUMEN_SYSV_ABI*)(int, const char*, void*, int, int, int);
using Send = int (LUMEN_SYSV_ABI*)(int, const void*, uint64_t, int);
using SendTo = int (LUMEN_SYSV_ABI*)(int, const void*, uint64_t, int, const void*, uint32_t);
using Setsockopt = int (LUMEN_SYSV_ABI*)(int, int, int, const void*, uint32_t);
using Socket = int (LUMEN_SYSV_ABI*)(const char*, int, int, int);

struct Binding
{
	const char* nid;
	uint32_t arity;
};

constexpr std::array<Binding, 31> BINDINGS {{
    {"Nlev7Lg8k3A", 0}, {"dgJBaeJnGpo", 3}, {"K7RlrTkI-mw", 1}, {"PIWqhn9oSxc", 3},
    {"bErx49PgxyY", 3}, {"OXXX4mUk3uk", 3}, {"w21YgGGNtBk", 2}, {"ZVw46bsasAk", 4},
    {"SF47kB2MNTo", 2}, {"Inp1lfL+Jdw", 1}, {"drjIbDbA7UQ", 4}, {"HQOwnfMGipQ", 0},
    {"hLuXdjHnhiI", 4}, {"hoOAofhhRvE", 3}, {"xphrZusl78E", 5}, {"iWQWrwiSt8A", 1},
    {"8Kcp5d-q1Uo", 3}, {"kOj1HiAGE54", 2}, {"9wO9XrMsNhc", 4}, {"304ooNZxWDY", 6},
    {"C4UgDHHPvdw", 3}, {"kJlYH5uMAWI", 1}, {"J5i3hiLJMPk", 2}, {"Apb4YDxKsRI", 7},
    {"Nd91WaWmG2w", 6}, {"beRjXBn-z+o", 4}, {"gvD1greCu0A", 6}, {"2mKX2Spso7I", 5},
    {"Q4qBuN-c0ZM", 4}, {"45ggEzakPJQ", 1}, {"cTGkc6-TBlI", 0},
}};

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NetOfflineSurfaceTest: %s\n", message);
	return condition;
}

SymbolResolve Exact(const char* nid)
{
	SymbolResolve query {};
	query.name = nid;
	query.library = "libSceNet";
	query.library_version = 1;
	query.module = "libSceNet";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = SymbolType::Func;
	return query;
}

template <typename Function> Function Resolve(const SymbolDatabase& symbols, const char* nid)
{
	const auto* record = symbols.Find(Exact(nid));
	if (record == nullptr)
	{
		std::fprintf(stderr, "NetOfflineSurfaceTest: unresolved NID %s\n", nid);
		return nullptr;
	}
	return reinterpret_cast<Function>(record->vaddr);
}

bool CheckBindings(const SymbolDatabase& symbols)
{
	bool passed = true;
	for (const auto& binding: BINDINGS)
	{
		const auto* record = symbols.Find(Exact(binding.nid));
		if (record == nullptr)
		{
			std::fprintf(stderr, "NetOfflineSurfaceTest: missing binding %s\n", binding.nid);
			passed = false;
			continue;
		}
		const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
		if (metadata == nullptr || metadata->arity != binding.arity)
		{
			std::fprintf(stderr, "NetOfflineSurfaceTest: wrong scalar arity for %s\n", binding.nid);
			passed = false;
		}

		auto wrong_library = Exact(binding.nid);
		wrong_library.library = "unrelated_network_library";
		auto wrong_module = Exact(binding.nid);
		wrong_module.module = "unrelated_network_module";
		auto wrong_version = Exact(binding.nid);
		wrong_version.module_version_minor = 0;
		if (symbols.Find(wrong_library) != nullptr || symbols.Find(wrong_module) != nullptr ||
		    symbols.Find(wrong_version) != nullptr)
		{
			std::fprintf(stderr, "NetOfflineSurfaceTest: over-broad identity for %s\n", binding.nid);
			passed = false;
		}
	}
	return Check(symbols.FindByStdName(U"sceNetSocket") == nullptr,
	             "libSceNet gained a standard-name fallback") && passed;
}

template <typename Function, typename... Arguments>
bool ExpectOffline(ErrorLocation error_location, Function function, const char* name, Arguments... arguments)
{
	constexpr int sentinel = 0x31579;
	*error_location() = sentinel;
	const int result = function(arguments...);
	if (result == Lumen::Libs::Network::NET_ERROR_ENETDOWN &&
	    *error_location() == Lumen::Libs::Network::NetToPosix(result))
	{
		return true;
	}
	std::fprintf(stderr, "NetOfflineSurfaceTest: %s returned 0x%08x errno=0x%08x\n", name,
	             static_cast<unsigned>(result), static_cast<unsigned>(*error_location()));
	return false;
}

bool CheckAdministrativeCalls(const SymbolDatabase& symbols, ErrorLocation error_location)
{
	const auto init = Resolve<NoArguments>(symbols, "Nlev7Lg8k3A");
	const auto create_pool = Resolve<PoolCreate>(symbols, "dgJBaeJnGpo");
	const auto destroy_pool = Resolve<OneInt>(symbols, "K7RlrTkI-mw");
	const auto htons = Resolve<Htons>(symbols, "iWQWrwiSt8A");
	const auto inet_pton = Resolve<InetPton>(symbols, "8Kcp5d-q1Uo");
	const auto term = Resolve<NoArguments>(symbols, "cTGkc6-TBlI");
	if (init == nullptr || create_pool == nullptr || destroy_pool == nullptr || htons == nullptr ||
	    inet_pton == nullptr || term == nullptr)
		return false;

	constexpr int sentinel = 0x2468;
	*error_location() = sentinel;
	if (!Check(init() == 0, "NetInit did not preserve administrative success") ||
	    !Check(*error_location() == sentinel, "NetInit changed net errno") ||
	    !Check(htons(0x1234u) == 0x3412u, "NetHtons did not byte-swap") ||
	    !Check(*error_location() == sentinel, "NetHtons changed net errno"))
		return false;

	std::array<uint8_t, 4> address {{0xa5u, 0xa5u, 0xa5u, 0xa5u}};
	if (!Check(inet_pton(2, "127.0.0.1", address.data()) == 1,
	           "NetInetPton rejected a valid IPv4 address") ||
	    !Check(address == std::array<uint8_t, 4> {{127u, 0u, 0u, 1u}},
	           "NetInetPton wrote non-network-order bytes") ||
	    !Check(*error_location() == sentinel, "successful NetInetPton changed net errno"))
		return false;
	address.fill(0xa5u);
	if (!Check(inet_pton(2, "256.0.0.1", address.data()) == 0,
	           "NetInetPton accepted an invalid IPv4 address") ||
	    !Check(address == std::array<uint8_t, 4> {{0xa5u, 0xa5u, 0xa5u, 0xa5u}},
	           "failed NetInetPton modified its output") ||
	    !Check(*error_location() == sentinel, "invalid-text NetInetPton changed net errno"))
		return false;
	if (!Check(inet_pton(28, "::1", address.data()) == Lumen::Libs::Network::NET_ERROR_EAFNOSUPPORT,
	           "NetInetPton did not reject an unsupported family") ||
	    !Check(*error_location() ==
	               Lumen::Libs::Network::NetToPosix(Lumen::Libs::Network::NET_ERROR_EAFNOSUPPORT),
	           "NetInetPton did not publish its family error"))
		return false;

	const int pool = create_pool("lumen-offline-test", 0x4000, 0);
	if (!Check(pool >= 0, "NetPoolCreate failed for a valid local pool") ||
	    !Check(destroy_pool(pool) == 0, "NetPoolDestroy rejected a live local pool") ||
	    !Check(destroy_pool(pool) == Lumen::Libs::Network::NET_ERROR_EBADF,
	           "NetPoolDestroy accepted a stale pool") ||
	    !Check(*error_location() ==
	               Lumen::Libs::Network::NetToPosix(Lumen::Libs::Network::NET_ERROR_EBADF),
	           "NetPoolDestroy did not publish its failure") ||
	    !Check(term() == 0, "NetTerm did not preserve administrative success"))
		return false;
	return true;
}

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Vm::Alloc(0, size, Vm::Mode::ReadWrite)) {}
	~Mapping()
	{
		if (m_address != 0 && !Vm::Free(m_address)) std::abort();
	}
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	bool Valid() const { return m_address != 0; }
	bool Protect(size_t offset, size_t size, Vm::Mode mode) const
	{
		return Valid() && offset <= m_size && size <= m_size - offset &&
		       Vm::Protect(Address(offset), size, mode);
	}

private:
	size_t m_size;
	uint64_t m_address;
};

bool CheckInetPtonGuestMemory(const SymbolDatabase& symbols, ErrorLocation error_location)
{
	constexpr size_t unit = 65536;
	Mapping memory(3 * unit);
	const auto inet_pton = Resolve<InetPton>(symbols, "8Kcp5d-q1Uo");
	if (!Check(memory.Valid() && inet_pton != nullptr, "NetInetPton VM fixture failed")) return false;

	const uint64_t crossing_input = memory.Address(unit - 5);
	const uint64_t ordinary_input = memory.Address(32);
	const uint64_t ordinary_output = memory.Address(2 * unit + 32);
	const uint64_t crossing_output = memory.Address(2 * unit - 2);
	std::memcpy(reinterpret_cast<void*>(crossing_input), "127.0.0.1", 10);
	std::memcpy(reinterpret_cast<void*>(ordinary_input), "10.20.30.40", 12);
	std::memset(reinterpret_cast<void*>(ordinary_output), 0xa5, 4);

	*error_location() = 0x5432;
	if (!Check(inet_pton(2, reinterpret_cast<const char*>(crossing_input),
	                     reinterpret_cast<void*>(ordinary_output)) == 1,
	           "NetInetPton rejected readable cross-boundary input") ||
	    !Check(std::memcmp(reinterpret_cast<const void*>(ordinary_output),
	                       std::array<uint8_t, 4> {{127, 0, 0, 1}}.data(), 4) == 0,
	           "NetInetPton cross-boundary conversion differs"))
		return false;

	std::memset(reinterpret_cast<void*>(ordinary_output), 0xa5, 4);
	if (!Check(memory.Protect(unit, unit, Vm::Mode::NoAccess), "input guard setup failed")) return false;
	const int guarded_input = inet_pton(2, reinterpret_cast<const char*>(crossing_input),
	                                   reinterpret_cast<void*>(ordinary_output));
	if (!Check(memory.Protect(unit, unit, Vm::Mode::ReadWrite), "input guard restoration failed")) return false;
	if (!Check(guarded_input == Lumen::Libs::Network::NET_ERROR_EFAULT,
	           "NetInetPton accepted input crossing into a guard") ||
	    !Check(*error_location() ==
	               Lumen::Libs::Network::NetToPosix(Lumen::Libs::Network::NET_ERROR_EFAULT),
	           "guarded input did not publish EFAULT") ||
	    !Check(std::memcmp(reinterpret_cast<const void*>(ordinary_output),
	                       std::array<uint8_t, 4> {{0xa5, 0xa5, 0xa5, 0xa5}}.data(), 4) == 0,
	           "guarded input modified output"))
		return false;

	std::memset(reinterpret_cast<void*>(crossing_output), 0xa5, 4);
	if (!Check(memory.Protect(2 * unit, unit, Vm::Mode::Read), "output read-only setup failed")) return false;
	const int guarded_output = inet_pton(2, reinterpret_cast<const char*>(ordinary_input),
	                                    reinterpret_cast<void*>(crossing_output));
	if (!Check(memory.Protect(2 * unit, unit, Vm::Mode::ReadWrite), "output protection restoration failed")) return false;
	if (!Check(guarded_output == Lumen::Libs::Network::NET_ERROR_EFAULT,
	           "NetInetPton accepted partially read-only output") ||
	    !Check(std::memcmp(reinterpret_cast<const void*>(crossing_output),
	                       std::array<uint8_t, 4> {{0xa5, 0xa5, 0xa5, 0xa5}}.data(), 4) == 0,
	           "NetInetPton partially wrote a rejected output"))
		return false;

	std::memset(reinterpret_cast<void*>(ordinary_output), 0xa5, 4);
	if (!Check(inet_pton(2, reinterpret_cast<const char*>(UINT64_C(1)),
	                     reinterpret_cast<void*>(ordinary_output)) ==
	               Lumen::Libs::Network::NET_ERROR_EFAULT,
	           "NetInetPton accepted an unmapped input") ||
	    !Check(inet_pton(2, reinterpret_cast<const char*>(ordinary_input),
	                     reinterpret_cast<void*>(UINT64_C(1))) ==
	               Lumen::Libs::Network::NET_ERROR_EFAULT,
	           "NetInetPton accepted an unmapped output") ||
	    !Check(std::memcmp(reinterpret_cast<const void*>(ordinary_output),
	                       std::array<uint8_t, 4> {{0xa5, 0xa5, 0xa5, 0xa5}}.data(), 4) == 0,
	           "unmapped NetInetPton call modified valid memory"))
		return false;
	return true;
}
#endif

bool CheckOfflineCalls(const SymbolDatabase& symbols, ErrorLocation error_location)
{
	std::array<uint8_t, 32> bytes {};
	bytes.fill(0x5au);
	const auto original = bytes;
	uint32_t length = 0x11223344u;
	int32_t resolver_status = 0x13572468;
	char hostname[16];
	std::memset(hostname, 0x6b, sizeof(hostname));
	const std::array<char, sizeof(hostname)> original_hostname = [] {
		std::array<char, sizeof(hostname)> value {};
		value.fill(static_cast<char>(0x6b));
		return value;
	}();

	bool passed = true;
	passed = ExpectOffline(error_location, Resolve<Accept>(symbols, "PIWqhn9oSxc"), "NetAccept",
	                       -1, bytes.data(), &length) && passed;
	passed = ExpectOffline(error_location, Resolve<AddressOperation>(symbols, "bErx49PgxyY"), "NetBind",
	                       -1, bytes.data(), static_cast<uint32_t>(bytes.size())) && passed;
	passed = ExpectOffline(error_location, Resolve<AddressOperation>(symbols, "OXXX4mUk3uk"), "NetConnect",
	                       -1, bytes.data(), static_cast<uint32_t>(bytes.size())) && passed;
	passed = ExpectOffline(error_location, Resolve<Listen>(symbols, "w21YgGGNtBk"), "NetEpollAbort",
	                       -1, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<EpollControl>(symbols, "ZVw46bsasAk"), "NetEpollControl",
	                       -1, 1, -1, bytes.data()) && passed;
	passed = ExpectOffline(error_location, Resolve<EpollCreate>(symbols, "SF47kB2MNTo"), "NetEpollCreate",
	                       "offline", 0) && passed;
	passed = ExpectOffline(error_location, Resolve<OneInt>(symbols, "Inp1lfL+Jdw"), "NetEpollDestroy", -1) && passed;
	passed = ExpectOffline(error_location, Resolve<EpollWait>(symbols, "drjIbDbA7UQ"), "NetEpollWait",
	                       -1, bytes.data(), 1, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<GetSockInfo>(symbols, "hLuXdjHnhiI"), "NetGetSockInfo",
	                       -1, bytes.data(), 1, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<Getsockname>(symbols, "hoOAofhhRvE"), "NetGetsockname",
	                       -1, bytes.data(), &length) && passed;
	passed = ExpectOffline(error_location, Resolve<Getsockopt>(symbols, "xphrZusl78E"), "NetGetsockopt",
	                       -1, 0, 0, bytes.data(), &length) && passed;
	passed = ExpectOffline(error_location, Resolve<Listen>(symbols, "kOj1HiAGE54"), "NetListen", -1, 1) && passed;
	passed = ExpectOffline(error_location, Resolve<Receive>(symbols, "9wO9XrMsNhc"), "NetRecv",
	                       -1, bytes.data(), bytes.size(), 0) && passed;
	passed = ExpectOffline(error_location, Resolve<ReceiveFrom>(symbols, "304ooNZxWDY"), "NetRecvfrom",
	                       -1, bytes.data(), bytes.size(), 0, bytes.data(), &length) && passed;
	passed = ExpectOffline(error_location, Resolve<ResolverCreate>(symbols, "C4UgDHHPvdw"), "NetResolverCreate",
	                       "offline", -1, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<OneInt>(symbols, "kJlYH5uMAWI"), "NetResolverDestroy", -1) && passed;
	passed = ExpectOffline(error_location, Resolve<ResolverGetError>(symbols, "J5i3hiLJMPk"), "NetResolverGetError",
	                       -1, &resolver_status) && passed;
	passed = ExpectOffline(error_location, Resolve<ResolverStartAton>(symbols, "Apb4YDxKsRI"), "NetResolverStartAton",
	                       -1, bytes.data(), hostname, static_cast<int>(sizeof(hostname)), 0, 0, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<ResolverStartNtoa>(symbols, "Nd91WaWmG2w"), "NetResolverStartNtoa",
	                       -1, "localhost", bytes.data(), 0, 0, 0) && passed;
	passed = ExpectOffline(error_location, Resolve<Send>(symbols, "beRjXBn-z+o"), "NetSend",
	                       -1, bytes.data(), bytes.size(), 0) && passed;
	passed = ExpectOffline(error_location, Resolve<SendTo>(symbols, "gvD1greCu0A"), "NetSendto",
	                       -1, bytes.data(), bytes.size(), 0, bytes.data(), static_cast<uint32_t>(bytes.size())) && passed;
	passed = ExpectOffline(error_location, Resolve<Setsockopt>(symbols, "2mKX2Spso7I"), "NetSetsockopt",
	                       -1, 0, 0, bytes.data(), static_cast<uint32_t>(bytes.size())) && passed;
	passed = ExpectOffline(error_location, Resolve<Socket>(symbols, "Q4qBuN-c0ZM"), "NetSocket",
	                       "offline", 2, 1, 0) && passed;

	const auto close_socket = Resolve<OneInt>(symbols, "45ggEzakPJQ");
	*error_location() = 0x7777;
	passed = Check(close_socket != nullptr && close_socket(-1) == Lumen::Libs::Network::NET_ERROR_EBADF,
	               "NetSocketClose did not reject an absent socket") && passed;
	passed = Check(*error_location() ==
	                   Lumen::Libs::Network::NetToPosix(Lumen::Libs::Network::NET_ERROR_EBADF),
	               "NetSocketClose did not publish EBADF") && passed;
	passed = Check(bytes == original && length == 0x11223344u && resolver_status == 0x13572468,
	               "offline failure modified an output buffer") && passed;
	passed = Check(std::memcmp(hostname, original_hostname.data(), sizeof(hostname)) == 0,
	               "offline resolver failure modified its hostname output") && passed;
	return passed;
}

bool CheckThreadLocalError(const SymbolDatabase& symbols, ErrorLocation error_location)
{
	const auto socket = Resolve<Socket>(symbols, "Q4qBuN-c0ZM");
	if (socket == nullptr) return false;
	constexpr int main_sentinel = 0x45ab;
	*error_location() = main_sentinel;
	int worker_before = 0;
	int worker_after = 0;
	std::thread worker([&] {
		worker_before = *error_location();
		(void)socket("worker", 2, 1, 0);
		worker_after = *error_location();
	});
	worker.join();
	return Check(worker_before == 0, "new thread inherited net errno") &&
	       Check(worker_after ==
	                 Lumen::Libs::Network::NetToPosix(Lumen::Libs::Network::NET_ERROR_ENETDOWN),
	             "worker did not retain its net error") &&
	       Check(*error_location() == main_sentinel, "worker changed main-thread net errno");
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Vm::Init();
	Lumen::Libs::Network::NetworkSubsystem::Instance()->Init(nullptr);

	SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libNet_1", &symbols), "libNet_1 registration failed")) return 1;
	const auto error_location = Resolve<ErrorLocation>(symbols, "HQOwnfMGipQ");
	if (!Check(error_location != nullptr && error_location() != nullptr,
	           "NetErrnoLoc did not return storage"))
		return 1;

	if (!CheckBindings(symbols) || !CheckAdministrativeCalls(symbols, error_location) ||
	    !CheckOfflineCalls(symbols, error_location) || !CheckThreadLocalError(symbols, error_location))
		return 1;
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	if (!CheckInetPtonGuestMemory(symbols, error_location)) return 1;
#endif

	std::puts("NetOfflineSurfaceTest: PASS");
	return 0;
}
