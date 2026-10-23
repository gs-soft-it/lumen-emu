#ifndef EMULATOR_INCLUDE_EMULATOR_NETWORK_H_
#define EMULATOR_INCLUDE_EMULATOR_NETWORK_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Network {

LUMEN_SUBSYSTEM_DEFINE(Network);

// Clear the emulated session's pools, contexts and callback-bearing HTTP
// objects without destroying the process-owned Network instance. The caller
// must have already stopped all guest threads. Returns false when Network has
// not been initialized.
[[nodiscard]] bool ResetForNextSession();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool NetworkSessionResetContractTest();
#endif

namespace Net {

struct NetEtherAddr;

int LUMEN_SYSV_ABI NetInit();
int LUMEN_SYSV_ABI NetTerm();
int LUMEN_SYSV_ABI NetPoolCreate(const char* name, int size, int flags);
int LUMEN_SYSV_ABI NetPoolDestroy(int memid);
int LUMEN_SYSV_ABI NetAccept(int socket, void* address, uint32_t* address_length);
int LUMEN_SYSV_ABI NetBind(int socket, const void* address, uint32_t address_length);
int LUMEN_SYSV_ABI NetConnect(int socket, const void* address, uint32_t address_length);
// The EpollAbort and GetSockInfo argument counts are local fail-closed scalar
// bridge models; they do not claim PS4 ABI equivalence and consume no buffers.
int LUMEN_SYSV_ABI NetEpollAbort(int epoll, int flags);
int LUMEN_SYSV_ABI NetEpollControl(int epoll, int operation, int socket, void* event);
int LUMEN_SYSV_ABI NetEpollCreate(const char* name, int flags);
int LUMEN_SYSV_ABI NetEpollDestroy(int epoll);
int LUMEN_SYSV_ABI NetEpollWait(int epoll, void* events, int maximum_events, int timeout);
int LUMEN_SYSV_ABI NetGetSockInfo(int socket, void* info, int count, int flags);
int LUMEN_SYSV_ABI NetGetsockname(int socket, void* address, uint32_t* address_length);
int LUMEN_SYSV_ABI NetGetsockopt(int socket, int level, int option_name, void* option_value, uint32_t* option_length);
uint16_t LUMEN_SYSV_ABI NetHtons(uint16_t host_value);
int LUMEN_SYSV_ABI NetInetPton(int af, const char* src, void* dst);
int LUMEN_SYSV_ABI NetListen(int socket, int backlog);
int LUMEN_SYSV_ABI NetRecv(int socket, void* buffer, uint64_t length, int flags);
int LUMEN_SYSV_ABI NetRecvfrom(int socket, void* buffer, uint64_t length, int flags, void* address, uint32_t* address_length);
// Resolver argument counts are a bounded local scalar-bridge model. The
// offline implementation never consumes these pointers and does not claim
// console-equivalent resolver behavior.
int LUMEN_SYSV_ABI NetResolverCreate(const char* name, int pool, int flags);
int LUMEN_SYSV_ABI NetResolverDestroy(int resolver);
int LUMEN_SYSV_ABI NetResolverGetError(int resolver, int32_t* status);
int LUMEN_SYSV_ABI NetResolverStartAton(int resolver, const void* address, char* hostname, int hostname_length, int timeout, int retry, int flags);
int LUMEN_SYSV_ABI NetResolverStartNtoa(int resolver, const char* hostname, void* address, int timeout, int retry, int flags);
int LUMEN_SYSV_ABI NetSend(int socket, const void* buffer, uint64_t length, int flags);
int LUMEN_SYSV_ABI NetSendto(int socket, const void* buffer, uint64_t length, int flags, const void* address, uint32_t address_length);
int LUMEN_SYSV_ABI NetSetsockopt(int socket, int level, int option_name, const void* option_value, uint32_t option_length);
int LUMEN_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol);
int LUMEN_SYSV_ABI NetSocketClose(int socket);
int LUMEN_SYSV_ABI NetEtherNtostr(const NetEtherAddr* n, char* str, size_t len);
int LUMEN_SYSV_ABI NetGetMacAddress(NetEtherAddr* addr, int flags);

} // namespace Net

namespace Ssl {

int LUMEN_SYSV_ABI SslInit(uint64_t pool_size);
int LUMEN_SYSV_ABI SslTerm(int ssl_ctx_id);

} // namespace Ssl

namespace Http {

struct HttpEpoll;

using HttpEpollHandle = HttpEpoll*;

using HttpsCallback = LUMEN_SYSV_ABI int (*)(int, unsigned int, void* const*, int, void*);

int LUMEN_SYSV_ABI HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size);
int LUMEN_SYSV_ABI HttpTerm(int http_ctx_id);
int LUMEN_SYSV_ABI HttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver, int is_auto_proxy_conf);
int LUMEN_SYSV_ABI HttpDeleteTemplate(int tmpl_id);
int LUMEN_SYSV_ABI HttpSetNonblock(int id, int enable);
int LUMEN_SYSV_ABI HttpCreateEpoll(int http_ctx_id, HttpEpollHandle* eh);
int LUMEN_SYSV_ABI HttpDestroyEpoll(int http_ctx_id, HttpEpollHandle eh);
int LUMEN_SYSV_ABI HttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive);
int LUMEN_SYSV_ABI HttpDeleteConnection(int conn_id);
int LUMEN_SYSV_ABI HttpCreateRequestWithURL2(int conn_id, const char* method, const char* url, uint64_t content_length);
int LUMEN_SYSV_ABI HttpDeleteRequest(int req_id);
int LUMEN_SYSV_ABI HttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode);
int LUMEN_SYSV_ABI HttpSetEpoll(int id, HttpEpollHandle eh, void* user_arg);
int LUMEN_SYSV_ABI HttpUnsetEpoll(int id);
int LUMEN_SYSV_ABI HttpSendRequest(int request_id, const void* post_data, size_t size);
int LUMEN_SYSV_ABI HttpsSetSslCallback(int id, HttpsCallback cbfunc, void* user_arg);
int LUMEN_SYSV_ABI HttpsDisableOption(int id, uint32_t ssl_flags);
int LUMEN_SYSV_ABI HttpSetResolveTimeOut(int id, uint32_t usec);
int LUMEN_SYSV_ABI HttpSetResolveRetry(int id, int32_t retry);
int LUMEN_SYSV_ABI HttpSetConnectTimeOut(int id, uint32_t usec);
int LUMEN_SYSV_ABI HttpSetSendTimeOut(int id, uint32_t usec);
int LUMEN_SYSV_ABI HttpSetRecvTimeOut(int id, uint32_t usec);
int LUMEN_SYSV_ABI HttpSetAutoRedirect(int id, int enable);
int LUMEN_SYSV_ABI HttpSetAuthEnabled(int id, int enable);

} // namespace Http

namespace NetCtl {

struct NetCtlNatInfo;
union NetCtlInfo;

using NetCtlCallback = LUMEN_SYSV_ABI void (*)(int, void*);

int LUMEN_SYSV_ABI  NetCtlInit();
void LUMEN_SYSV_ABI NetCtlTerm();
int LUMEN_SYSV_ABI  NetCtlGetNatInfo(NetCtlNatInfo* nat_info);
int LUMEN_SYSV_ABI  NetCtlCheckCallback();
int LUMEN_SYSV_ABI  NetCtlGetState(int* state);
int LUMEN_SYSV_ABI  NetCtlRegisterCallback(NetCtlCallback func, void* arg, int* cid);
int LUMEN_SYSV_ABI  NetCtlGetInfo(int code, NetCtlInfo* info);

} // namespace NetCtl

namespace NpManager {

struct NpTitleId;
struct NpTitleSecret;
struct NpContentRestriction;
struct NpId;
struct NpOnlineId;
struct NpCreateAsyncRequestParameter;
struct NpAccountId;

int LUMEN_SYSV_ABI  NpCheckCallback();
int LUMEN_SYSV_ABI  NpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret);
int LUMEN_SYSV_ABI  NpSetContentRestriction(const NpContentRestriction* restriction);
int LUMEN_SYSV_ABI  NpRegisterStateCallback(void* callback, void* userdata);
void LUMEN_SYSV_ABI NpRegisterGamePresenceCallback(void* callback, void* userdata);
int LUMEN_SYSV_ABI  NpRegisterPlusEventCallback(void* callback, void* userdata);
int LUMEN_SYSV_ABI  NpRegisterNpReachabilityStateCallback(void* callback, void* userdata);
int LUMEN_SYSV_ABI  NpGetNpId(int user_id, NpId* np_id);
int LUMEN_SYSV_ABI  NpGetOnlineId(int user_id, NpOnlineId* online_id);
int LUMEN_SYSV_ABI  NpGetAccountIdA(int user_id, NpAccountId* account_id);
int LUMEN_SYSV_ABI  NpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param);
int LUMEN_SYSV_ABI  NpAbortRequest(int req_id);
int LUMEN_SYSV_ABI  NpDeleteRequest(int req_id);
int LUMEN_SYSV_ABI  NpCheckNpAvailability(int req_id, const char* user, void* result);
int LUMEN_SYSV_ABI  NpPollAsync(int req_id, int* result);
int LUMEN_SYSV_ABI  NpGetState(int user_id, uint32_t* state);

} // namespace NpManager

namespace NpCommon {

int LUMEN_SYSV_ABI NpCmpNpId(const NpManager::NpId* left, const NpManager::NpId* right);

} // namespace NpCommon

namespace NpManagerForToolkit {

int LUMEN_SYSV_ABI NpRegisterStateCallbackForToolkit(void* callback, void* userdata);
int LUMEN_SYSV_ABI NpCheckCallbackForLib();

} // namespace NpManagerForToolkit

namespace NpTrophy {

struct NpTrophyFlagArray;
struct NpTrophyGameDetails;
struct NpTrophyGameData;

int LUMEN_SYSV_ABI NpTrophyCreateHandle(int* handle);
int LUMEN_SYSV_ABI NpTrophyCreateContext(int* context, int user_id, uint32_t service_label, uint64_t options);
int LUMEN_SYSV_ABI NpTrophyRegisterContext(int context, int handle, uint64_t options);
int LUMEN_SYSV_ABI NpTrophyDestroyHandle(int handle);
int LUMEN_SYSV_ABI NpTrophyGetGameInfo(int context, int handle, NpTrophyGameDetails* details, NpTrophyGameData* data);
int LUMEN_SYSV_ABI NpTrophyGetTrophyUnlockState(int context, int handle, NpTrophyFlagArray* flags, uint32_t* count);
int LUMEN_SYSV_ABI NpTrophyUnlockTrophy(int context, int handle, int trophy_id, int* platinum_id);

} // namespace NpTrophy

namespace NpWebApi {

int LUMEN_SYSV_ABI NpWebApiInitialize(int http_ctx_id, size_t pool_size);
int LUMEN_SYSV_ABI NpWebApiTerminate(int lib_ctx_id);

} // namespace NpWebApi

} // namespace Lumen::Libs::Network

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_NETWORK_H_ */
