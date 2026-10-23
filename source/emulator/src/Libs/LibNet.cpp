#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Network.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpInGameMessage.h"
#include "Emulator/Libs/Rudp.h"

#include <cstdio>

#ifdef LUMEN_EMU_ENABLED

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NET_CALL(func)                                                                                                                     \
	[&]()                                                                                                                                  \
	{                                                                                                                                      \
		auto result = func;                                                                                                                \
		if (result < 0)                                                                                                                    \
		{                                                                                                                                  \
			*GetNetErrorAddr() = Network::NetToPosix(result);                                                                               \
		}                                                                                                                                  \
		return result;                                                                                                                     \
	}()

namespace Lumen::Libs {

namespace Network::Net {
struct NetEtherAddr;
} // namespace Network::Net

namespace LibNet {

LIB_VERSION("Net", 1, "Net", 1, 1);

static thread_local int g_net_errno = 0;

namespace Net = Network::Net;

LUMEN_SYSV_ABI int* GetNetErrorAddr()
{
	return &g_net_errno;
}

static int LUMEN_SYSV_ABI NetInit()
{
	return NET_CALL(Net::NetInit());
}

static int LUMEN_SYSV_ABI NetTerm()
{
	return NET_CALL(Net::NetTerm());
}

int LUMEN_SYSV_ABI NetPoolCreate(const char* name, int size, int flags)
{
	return NET_CALL(Net::NetPoolCreate(name, size, flags));
}

static int LUMEN_SYSV_ABI NetPoolDestroy(int memory_pool)
{
	return NET_CALL(Net::NetPoolDestroy(memory_pool));
}

static int LUMEN_SYSV_ABI NetAccept(int socket, void* address, uint32_t* address_length)
{
	return NET_CALL(Net::NetAccept(socket, address, address_length));
}

static int LUMEN_SYSV_ABI NetBind(int socket, const void* address, uint32_t address_length)
{
	return NET_CALL(Net::NetBind(socket, address, address_length));
}

static int LUMEN_SYSV_ABI NetConnect(int socket, const void* address, uint32_t address_length)
{
	return NET_CALL(Net::NetConnect(socket, address, address_length));
}

static int LUMEN_SYSV_ABI NetEpollAbort(int epoll, int flags)
{
	return NET_CALL(Net::NetEpollAbort(epoll, flags));
}

static int LUMEN_SYSV_ABI NetEpollControl(int epoll, int operation, int socket, void* event)
{
	return NET_CALL(Net::NetEpollControl(epoll, operation, socket, event));
}

static int LUMEN_SYSV_ABI NetEpollCreate(const char* name, int flags)
{
	return NET_CALL(Net::NetEpollCreate(name, flags));
}

static int LUMEN_SYSV_ABI NetEpollDestroy(int epoll)
{
	return NET_CALL(Net::NetEpollDestroy(epoll));
}

static int LUMEN_SYSV_ABI NetEpollWait(int epoll, void* events, int maximum_events, int timeout)
{
	return NET_CALL(Net::NetEpollWait(epoll, events, maximum_events, timeout));
}

static int LUMEN_SYSV_ABI NetGetSockInfo(int socket, void* info, int count, int flags)
{
	return NET_CALL(Net::NetGetSockInfo(socket, info, count, flags));
}

static int LUMEN_SYSV_ABI NetGetsockname(int socket, void* address, uint32_t* address_length)
{
	return NET_CALL(Net::NetGetsockname(socket, address, address_length));
}

static int LUMEN_SYSV_ABI NetGetsockopt(int socket, int level, int option_name, void* option_value, uint32_t* option_length)
{
	return NET_CALL(Net::NetGetsockopt(socket, level, option_name, option_value, option_length));
}

static uint16_t LUMEN_SYSV_ABI NetHtons(uint16_t host_value)
{
	return Net::NetHtons(host_value);
}

int LUMEN_SYSV_ABI NetInetPton(int af, const char* src, void* dst)
{
	return NET_CALL(Net::NetInetPton(af, src, dst));
}

static int LUMEN_SYSV_ABI NetListen(int socket, int backlog)
{
	return NET_CALL(Net::NetListen(socket, backlog));
}

static int LUMEN_SYSV_ABI NetRecv(int socket, void* buffer, uint64_t length, int flags)
{
	return NET_CALL(Net::NetRecv(socket, buffer, length, flags));
}

static int LUMEN_SYSV_ABI NetRecvfrom(int socket, void* buffer, uint64_t length, int flags, void* address, uint32_t* address_length)
{
	return NET_CALL(Net::NetRecvfrom(socket, buffer, length, flags, address, address_length));
}

static int LUMEN_SYSV_ABI NetResolverCreate(const char* name, int pool, int flags)
{
	return NET_CALL(Net::NetResolverCreate(name, pool, flags));
}

static int LUMEN_SYSV_ABI NetResolverDestroy(int resolver)
{
	return NET_CALL(Net::NetResolverDestroy(resolver));
}

static int LUMEN_SYSV_ABI NetResolverGetError(int resolver, int32_t* status)
{
	return NET_CALL(Net::NetResolverGetError(resolver, status));
}

static int LUMEN_SYSV_ABI NetResolverStartAton(int resolver, const void* address, char* hostname,
                                               int hostname_length, int timeout, int retry, int flags)
{
	return NET_CALL(Net::NetResolverStartAton(resolver, address, hostname, hostname_length, timeout, retry, flags));
}

static int LUMEN_SYSV_ABI NetResolverStartNtoa(int resolver, const char* hostname, void* address,
                                               int timeout, int retry, int flags)
{
	return NET_CALL(Net::NetResolverStartNtoa(resolver, hostname, address, timeout, retry, flags));
}

static int LUMEN_SYSV_ABI NetSend(int socket, const void* buffer, uint64_t length, int flags)
{
	return NET_CALL(Net::NetSend(socket, buffer, length, flags));
}

static int LUMEN_SYSV_ABI NetSendto(int socket, const void* buffer, uint64_t length, int flags,
                                    const void* address, uint32_t address_length)
{
	return NET_CALL(Net::NetSendto(socket, buffer, length, flags, address, address_length));
}

static int LUMEN_SYSV_ABI NetSetsockopt(int socket, int level, int option_name, const void* option_value, uint32_t option_length)
{
	return NET_CALL(Net::NetSetsockopt(socket, level, option_name, option_value, option_length));
}

static int LUMEN_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol)
{
	return NET_CALL(Net::NetSocket(name, family, type, protocol));
}

static int LUMEN_SYSV_ABI NetSocketClose(int socket)
{
	return NET_CALL(Net::NetSocketClose(socket));
}

int LUMEN_SYSV_ABI NetEtherNtostr(const Net::NetEtherAddr* n, char* str, size_t len)
{
	return NET_CALL(Net::NetEtherNtostr(n, str, len));
}

int LUMEN_SYSV_ABI NetGetMacAddress(Net::NetEtherAddr* addr, int flags)
{
	return NET_CALL(Net::NetGetMacAddress(addr, flags));
}

LIB_DEFINE(InitNet_1_Net)
{
	LIB_FUNC_SCALAR("Nlev7Lg8k3A", LibNet::NetInit);
	LIB_FUNC_SCALAR("dgJBaeJnGpo", LibNet::NetPoolCreate);
	LIB_FUNC_SCALAR("K7RlrTkI-mw", LibNet::NetPoolDestroy);
	LIB_FUNC_SCALAR("PIWqhn9oSxc", LibNet::NetAccept);
	LIB_FUNC_SCALAR("bErx49PgxyY", LibNet::NetBind);
	LIB_FUNC_SCALAR("OXXX4mUk3uk", LibNet::NetConnect);
	LIB_FUNC_SCALAR("w21YgGGNtBk", LibNet::NetEpollAbort);
	LIB_FUNC_SCALAR("ZVw46bsasAk", LibNet::NetEpollControl);
	LIB_FUNC_SCALAR("SF47kB2MNTo", LibNet::NetEpollCreate);
	LIB_FUNC_SCALAR("Inp1lfL+Jdw", LibNet::NetEpollDestroy);
	LIB_FUNC_SCALAR("drjIbDbA7UQ", LibNet::NetEpollWait);
	LIB_FUNC_SCALAR("HQOwnfMGipQ", LibNet::GetNetErrorAddr);
	LIB_FUNC_SCALAR("hLuXdjHnhiI", LibNet::NetGetSockInfo);
	LIB_FUNC_SCALAR("hoOAofhhRvE", LibNet::NetGetsockname);
	LIB_FUNC_SCALAR("xphrZusl78E", LibNet::NetGetsockopt);
	LIB_FUNC_SCALAR("iWQWrwiSt8A", LibNet::NetHtons);
	LIB_FUNC_SCALAR("8Kcp5d-q1Uo", LibNet::NetInetPton);
	LIB_FUNC_SCALAR("kOj1HiAGE54", LibNet::NetListen);
	LIB_FUNC_SCALAR("9wO9XrMsNhc", LibNet::NetRecv);
	LIB_FUNC_SCALAR("304ooNZxWDY", LibNet::NetRecvfrom);
	LIB_FUNC_SCALAR("C4UgDHHPvdw", LibNet::NetResolverCreate);
	LIB_FUNC_SCALAR("kJlYH5uMAWI", LibNet::NetResolverDestroy);
	LIB_FUNC_SCALAR("J5i3hiLJMPk", LibNet::NetResolverGetError);
	LIB_FUNC_SCALAR("Apb4YDxKsRI", LibNet::NetResolverStartAton);
	LIB_FUNC_SCALAR("Nd91WaWmG2w", LibNet::NetResolverStartNtoa);
	LIB_FUNC_SCALAR("beRjXBn-z+o", LibNet::NetSend);
	LIB_FUNC_SCALAR("gvD1greCu0A", LibNet::NetSendto);
	LIB_FUNC_SCALAR("2mKX2Spso7I", LibNet::NetSetsockopt);
	LIB_FUNC_SCALAR("Q4qBuN-c0ZM", LibNet::NetSocket);
	LIB_FUNC_SCALAR("45ggEzakPJQ", LibNet::NetSocketClose);
	LIB_FUNC_SCALAR("cTGkc6-TBlI", LibNet::NetTerm);
	LIB_FUNC("v6M4txecCuo", LibNet::NetEtherNtostr);
	LIB_FUNC("6Oc0bLsIYe0", LibNet::NetGetMacAddress);
}

} // namespace LibNet

namespace LibRudp {

LIB_VERSION("Rudp", 1, "Rudp", 0, 0);

LIB_DEFINE(InitNet_1_Rudp)
{
	if (!Rudp::ExperimentalEnabled()) return;
	LIB_FUNC("amuBfI-AQc4", Rudp::Init);
	LIB_FUNC("6PBNpsgyaxw", Rudp::EnableInternalIOThread);
	std::fprintf(stderr,
	             "[RUDP_OFFLINE_EXPERIMENT] enabled=1 exports=2 experimental=1 "
	             "console_contract=unverified workspace=opaque-token scalar=opaque host_thread=0 networking=0\n");
}

} // namespace LibRudp

namespace LibSsl {

LIB_VERSION("Ssl", 1, "Ssl", 1, 1);

namespace Ssl = Network::Ssl;

LIB_DEFINE(InitNet_1_Ssl)
{
	LIB_FUNC("hdpVEUDFW3s", Ssl::SslInit);
	LIB_FUNC_SCALAR("0K1yQ6Lv-Yc", Ssl::SslTerm);
}

} // namespace LibSsl

namespace LibSsl2 {

// An observed import identity of the existing SSL context API. Keep this
// explicit; other module versions must not acquire a resolver fallback.
LIB_VERSION("Ssl", 1, "Ssl", 2, 1);

LIB_DEFINE(InitNet_1_Ssl2)
{
	LIB_FUNC_SCALAR("hdpVEUDFW3s", Network::Ssl::SslInit);
	LIB_FUNC_SCALAR("0K1yQ6Lv-Yc", Network::Ssl::SslTerm);
}

} // namespace LibSsl2

namespace LibHttp {

LIB_VERSION("Http", 1, "Http", 1, 1);

namespace Http = Network::Http;

LIB_DEFINE(InitNet_1_Http)
{
	LIB_FUNC("A9cVMUtEp4Y", Http::HttpInit);
	LIB_FUNC_SCALAR("Ik-KpLTlf7Q", Http::HttpTerm);
	LIB_FUNC("0gYjPTR-6cY", Http::HttpCreateTemplate);
	LIB_FUNC("4I8vEpuEhZ8", Http::HttpDeleteTemplate);
	LIB_FUNC("s2-NPIvz+iA", Http::HttpSetNonblock);
	LIB_FUNC("htyBOoWeS58", Http::HttpsSetSslCallback);
	LIB_FUNC("mSQCxzWTwVI", Http::HttpsDisableOption);
	LIB_FUNC("6381dWF+xsQ", Http::HttpCreateEpoll);
	LIB_FUNC("wYhXVfS2Et4", Http::HttpDestroyEpoll);
	LIB_FUNC("-xm7kZQNpHI", Http::HttpSetEpoll);
	LIB_FUNC("59tL1AQBb8U", Http::HttpUnsetEpoll);
	LIB_FUNC("qgxDBjorUxs", Http::HttpCreateConnectionWithURL);
	LIB_FUNC("P6A3ytpsiYc", Http::HttpDeleteConnection);
	LIB_FUNC("Cnp77podkCU", Http::HttpCreateRequestWithURL2);
	LIB_FUNC("qe7oZ+v4PWA", Http::HttpDeleteRequest);
	LIB_FUNC("EY28T2bkN7k", Http::HttpAddRequestHeader);
	LIB_FUNC("1e2BNwI-XzE", Http::HttpSendRequest);
	LIB_FUNC("Tc-hAYDKtQc", Http::HttpSetResolveTimeOut);
	LIB_FUNC("K1d1LqZRQHQ", Http::HttpSetResolveRetry);
	LIB_FUNC("0S9tTH0uqTU", Http::HttpSetConnectTimeOut);
	LIB_FUNC("xegFfZKBVlw", Http::HttpSetSendTimeOut);
	LIB_FUNC("yigr4V0-HTM", Http::HttpSetRecvTimeOut);
	LIB_FUNC("T-mGo9f3Pu4", Http::HttpSetAutoRedirect);
	LIB_FUNC("qFg2SuyTJJY", Http::HttpSetAuthEnabled);
}

} // namespace LibHttp

namespace LibNetCtl {

LIB_VERSION("NetCtl", 1, "NetCtl", 1, 1);

namespace NetCtl = Network::NetCtl;

LIB_DEFINE(InitNet_1_NetCtl)
{
	LIB_FUNC("gky0+oaNM4k", NetCtl::NetCtlInit);
	LIB_FUNC("JO4yuTuMoKI", NetCtl::NetCtlGetNatInfo);
	LIB_FUNC("iQw3iQPhvUQ", NetCtl::NetCtlCheckCallback);
	LIB_FUNC("uBPlr0lbuiI", NetCtl::NetCtlGetState);
	LIB_FUNC("UJ+Z7Q+4ck0", NetCtl::NetCtlRegisterCallback);
	LIB_FUNC("obuxdTiwkF8", NetCtl::NetCtlGetInfo);
}

} // namespace LibNetCtl

namespace LibNpCommon {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

LIB_DEFINE(InitNet_1_NpCommon)
{
	LIB_FUNC("i8UmXTSq7N4", Network::NpCommon::NpCmpNpId);
}

} // namespace LibNpCommon

namespace LibNpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace NpManager = Network::NpManager;

LIB_DEFINE(InitNet_1_NpManager)
{
	LIB_FUNC("3Zl8BePTh9Y", NpManager::NpCheckCallback);
	LIB_FUNC("Ec63y59l9tw", NpManager::NpSetNpTitleId);
	LIB_FUNC("A2CQ3kgSopQ", NpManager::NpSetContentRestriction);
	LIB_FUNC("VfRSmPmj8Q8", NpManager::NpRegisterStateCallback);
	LIB_FUNC("uFJpaKNBAj4", NpManager::NpRegisterGamePresenceCallback);
	LIB_FUNC("GImICnh+boA", NpManager::NpRegisterPlusEventCallback);
	LIB_FUNC("hw5KNqAAels", NpManager::NpRegisterNpReachabilityStateCallback);
	LIB_FUNC("p-o74CnoNzY", NpManager::NpGetNpId);
	LIB_FUNC("XDncXQIJUSk", NpManager::NpGetOnlineId);
	LIB_FUNC_SCALAR("rbknaUjpqWo", NpManager::NpGetAccountIdA);
	LIB_FUNC("eiqMCt9UshI", NpManager::NpCreateAsyncRequest);
	LIB_FUNC("OzKvTvg3ZYU", NpManager::NpAbortRequest);
	LIB_FUNC("S7QTn72PrDw", NpManager::NpDeleteRequest);
	LIB_FUNC("2rsFmlGWleQ", NpManager::NpCheckNpAvailability);
	LIB_FUNC("uqcPJLWL08M", NpManager::NpPollAsync);
	LIB_FUNC("eQH7nWPcAgc", NpManager::NpGetState);
	if (NpInGameMessage::ExperimentalEnabled())
	{
		LIB_FUNC("GFhVUpRmbHE", NpInGameMessage::Initialize);
		std::fprintf(stderr,
		             "[NP_IN_GAME_MESSAGE_OFFLINE_EXPERIMENT] enabled=1 exports=1 experimental=1 "
		             "console_contract=unverified scalar=opaque parameter=null-only networking=0\n");
	}
	if (NpPlus::ExperimentalEnabled())
	{
		LIB_FUNC("r6MyYJkryz8", NpPlus::CheckPlus);
		std::fprintf(stderr,
		             "[NP_PLUS_OFFLINE_EXPERIMENT] enabled=1 exports=1 experimental=1 "
		             "console_contract=unverified policy=deny-all\n");
	}
	if (NpParental::ExperimentalEnabled())
	{
		LIB_FUNC("ilwLM4zOmu4", NpParental::GetParentalControlInfo);
		std::fprintf(stderr,
		             "[NP_PARENTAL_OFFLINE_EXPERIMENT] enabled=1 exports=1 experimental=1 "
		             "console_contract=unverified policy=restricted-local-profile\n");
	}
}

} // namespace LibNpManager

namespace LibNpManagerForToolkit {

LIB_VERSION("NpManagerForToolkit", 1, "NpManager", 1, 1);

namespace NpManagerForToolkit = Network::NpManagerForToolkit;

LIB_DEFINE(InitNet_1_NpManagerForToolkit)
{
	LIB_FUNC("0c7HbXRKUt4", NpManagerForToolkit::NpRegisterStateCallbackForToolkit);
	LIB_FUNC("JELHf4xPufo", NpManagerForToolkit::NpCheckCallbackForLib);
}

} // namespace LibNpManagerForToolkit

namespace LibNpTrophy {

LIB_VERSION("NpTrophy", 1, "NpTrophy", 1, 1);

namespace NpTrophy = Network::NpTrophy;

LIB_DEFINE(InitNet_1_NpTrophy)
{
	LIB_FUNC("q7U6tEAQf7c", NpTrophy::NpTrophyCreateHandle);
	LIB_FUNC("XbkjbobZlCY", NpTrophy::NpTrophyCreateContext);
	LIB_FUNC("TJCAxto9SEU", NpTrophy::NpTrophyRegisterContext);
	LIB_FUNC("GNcF4oidY0Y", NpTrophy::NpTrophyDestroyHandle);
	LIB_FUNC("YYP3f2W09og", NpTrophy::NpTrophyGetGameInfo);
	LIB_FUNC("LHuSmO3SLd8", NpTrophy::NpTrophyGetTrophyUnlockState);
}

} // namespace LibNpTrophy

namespace LibNpWebApi {

LIB_VERSION("NpWebApi", 1, "NpWebApi", 1, 1);

namespace NpWebApi = Network::NpWebApi;

LIB_DEFINE(InitNet_1_NpWebApi)
{
	LIB_FUNC("G3AnLNdRBjE", NpWebApi::NpWebApiInitialize);
}

} // namespace LibNpWebApi

LIB_DEFINE(InitNet_1)
{
	LibNet::InitNet_1_Net(s);
	LibRudp::InitNet_1_Rudp(s);
	LibSsl::InitNet_1_Ssl(s);
	LibSsl2::InitNet_1_Ssl2(s);
	LibHttp::InitNet_1_Http(s);
	LibNetCtl::InitNet_1_NetCtl(s);
	LibNpCommon::InitNet_1_NpCommon(s);
	LibNpManager::InitNet_1_NpManager(s);
	LibNpManagerForToolkit::InitNet_1_NpManagerForToolkit(s);
	LibNpTrophy::InitNet_1_NpTrophy(s);
	LibNpWebApi::InitNet_1_NpWebApi(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
