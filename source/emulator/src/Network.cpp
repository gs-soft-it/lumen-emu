#include "Emulator/Network.h"

#include "Lumen/Core/ByteBuffer.h"
#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Libs/NpRequests.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Network {

namespace Http {
struct HttpEpoll;
}

class Network
{
public:
	class Id
	{
	public:
		static constexpr int MAX_ID = 65536;

		enum class Type : uint32_t
		{
			Invalid    = 0,
			Http       = 1,
			Ssl        = 2,
			Template   = 3,
			Connection = 4,
			Request    = 5,
		};

		explicit Id(int id): m_id(static_cast<uint32_t>(id) & 0xffffu), m_type(static_cast<uint32_t>(id) >> 16u) {}
		[[nodiscard]] int  ToInt() const { return static_cast<int>(m_id + (static_cast<uint32_t>(m_type) << 16u)); }
		[[nodiscard]] bool IsValid() const { return GetType() != Type::Invalid; }
		[[nodiscard]] Type GetType() const
		{
			switch (m_type)
			{
				case static_cast<uint32_t>(Type::Http): return Type::Http; break;
				case static_cast<uint32_t>(Type::Ssl): return Type::Ssl; break;
				case static_cast<uint32_t>(Type::Template): return Type::Template; break;
				case static_cast<uint32_t>(Type::Connection): return Type::Connection; break;
				case static_cast<uint32_t>(Type::Request): return Type::Request; break;
				default: return Type::Invalid;
			}
		}

		friend class Network;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int net_id, Type type)
		{
			Id r;
			r.m_id   = net_id;
			r.m_type = static_cast<uint32_t>(type);
			return r;
		}
		[[nodiscard]] int GetId() const { return static_cast<int>(m_id); }

		uint32_t m_id   = 0;
		uint32_t m_type = static_cast<uint32_t>(Type::Invalid);
	};

	using HttpsCallback = LUMEN_SYSV_ABI int (*)(int, unsigned int, void* const*, int, void*);

	Network()          = default;
	virtual ~Network() = default;

	LUMEN_CLASS_NO_COPY(Network);

	int  PoolCreate(const char* name, int size);
	bool PoolDestroy(int memid);

	Id   SslInit(uint64_t pool_size);
	bool SslTerm(Id ssl_ctx_id);

	Id   HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size);
	bool HttpTerm(Id http_ctx_id);
	Id   HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver, bool is_auto_proxy_conf);
	bool HttpDeleteTemplate(Id tmpl_id);
	bool HttpSetNonblock(Id id, bool enable);
	bool HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg);
	bool HttpsDisableOption(Id id, uint32_t ssl_flags);
	bool HttpAddRequestHeader(Id id, const char* name, const char* value, bool add);
	bool HttpValid(Id http_ctx_id);
	bool HttpValidTemplate(Id tmpl_id);
	bool HttpValidConnection(Id conn_id);
	bool HttpValidRequest(Id req_id);
	Id   HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive);
	bool HttpDeleteConnection(Id conn_id);
	Id   HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url, uint64_t content_length);
	bool HttpDeleteRequest(Id req_id);
	bool HttpSetResolveTimeOut(Id id, uint32_t usec);
	bool HttpSetResolveRetry(Id id, int32_t retry);
	bool HttpSetConnectTimeOut(Id id, uint32_t usec);
	bool HttpSetSendTimeOut(Id id, uint32_t usec);
	bool HttpSetRecvTimeOut(Id id, uint32_t usec);
	bool HttpSetAutoRedirect(Id id, int enable);
	bool HttpSetAuthEnabled(Id id, int enable);
	void ResetForNextSession();

	void TrackEpoll(::Lumen::Libs::Network::Http::HttpEpoll* epoll);
	bool UntrackEpoll(::Lumen::Libs::Network::Http::HttpEpoll* epoll);

	int CreateNpAsyncRequestId();
	bool HasIssuedNpAsyncRequestId(int id);
	int CreateNpWebApiId();

private:
	struct Pool
	{
		bool   used = false;
		String name;
		int    size = 0;
	};

	struct Ssl
	{
		bool     used = false;
		uint64_t size = 0;
	};

	struct Http
	{
		bool     used       = false;
		uint64_t size       = 0;
		int      memid      = 0;
		int      ssl_ctx_id = 0;
	};

	struct HttpHeader
	{
		String name;
		String value;
	};

	struct HttpBase
	{
		Vector<HttpHeader> headers;
		bool               used            = false;
		bool               nonblock        = false;
		bool               auto_redirect   = true;
		bool               auth_enabled    = true;
		HttpsCallback      ssl_cbfunc      = nullptr;
		void*              ssl_user_arg    = nullptr;
		uint32_t           ssl_flags       = 0xA7;
		int                http_ctx_id     = 0;
		uint32_t           resolve_timeout = 1'000000;
		int32_t            resolve_retry   = 4;
		uint32_t           connect_timeout = 30'000000;
		uint32_t           send_timeout    = 120'000000;
		uint32_t           recv_timeout    = 120'000000;
	};

	struct HttpTemplate: public HttpBase
	{
		String user_agent;
		int    http_ver           = 0;
		bool   is_auto_proxy_conf = true;
	};

	struct HttpConnection: public HttpTemplate
	{
		explicit HttpConnection(const HttpTemplate& tmpl): HttpTemplate(tmpl) {}
		// int    tmpl_id = 0;
		String url;
		bool   enable_keep_alive = false;
	};

	struct HttpRequest: public HttpConnection
	{
		explicit HttpRequest(HttpConnection& conn): HttpConnection(conn) {}
		// int      conn_id = 0;
		String   method;
		String   url;
		uint64_t content_length = 0;
	};

	static constexpr int POOLS_MAX = 32;
	static constexpr int SSL_MAX   = 32;
	static constexpr int HTTP_MAX  = 32;

	Core::Mutex              m_mutex;
	Pool                     m_pools[POOLS_MAX];
	Ssl                      m_ssl[SSL_MAX];
	Http                     m_http[HTTP_MAX];
	Vector<HttpTemplate>     m_templates;
	Vector<HttpConnection>   m_connections;
	Vector<HttpRequest>      m_requests;
	Vector<::Lumen::Libs::Network::Http::HttpEpoll*> m_epolls;
	int                       m_np_async_request_id = 0;
	int                       m_np_web_api_id       = 0;
};

static Network* g_net = nullptr;

LUMEN_SUBSYSTEM_INIT(Network)
{
	EXIT_IF(g_net != nullptr);

	g_net = new Network;
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Network)
{
	(void)ResetForNextSession();
}

LUMEN_SUBSYSTEM_DESTROY(Network)
{
	(void)ResetForNextSession();
}

bool ResetForNextSession()
{
	if (g_net == nullptr)
	{
		return false;
	}

	g_net->ResetForNextSession();
	return true;
}

int Network::PoolCreate(const char* name, int size)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < POOLS_MAX; id++)
	{
		if (!m_pools[id].used)
		{
			m_pools[id].used = true;
			m_pools[id].size = size;
			m_pools[id].name = String::FromUtf8(name);

			return id;
		}
	}

	return -1;
}

bool Network::PoolDestroy(int memid)
{
	Core::LockGuard lock(m_mutex);

	if (memid >= 0 && memid < POOLS_MAX && m_pools[memid].used)
	{
		m_pools[memid].used = false;

		return true;
	}

	return false;
}

Network::Id Network::SslInit(uint64_t pool_size)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < SSL_MAX; id++)
	{
		if (!m_ssl[id].used)
		{
			m_ssl[id].used = true;
			m_ssl[id].size = pool_size;

			return Id::Create(id, Id::Type::Ssl);
		}
	}

	return Id::Invalid();
}

bool Network::SslTerm(Id ssl_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 && ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used)
	{
		m_ssl[ssl_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size)
{
	Core::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 && ssl_ctx_id.GetId() < SSL_MAX &&
	    m_ssl[ssl_ctx_id.GetId()].used && memid >= 0 && memid < POOLS_MAX && m_pools[memid].used)
	{
		for (int id = 0; id < HTTP_MAX; id++)
		{
			if (!m_http[id].used)
			{
				m_http[id].used       = true;
				m_http[id].size       = pool_size;
				m_http[id].ssl_ctx_id = ssl_ctx_id.GetId();
				m_http[id].memid      = memid;

				return Id::Create(id, Id::Type::Http);
			}
		}
	}

	return Id::Invalid();
}

bool Network::HttpValid(Id http_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	return (http_ctx_id.GetType() == Id::Type::Http && http_ctx_id.GetId() >= 0 && http_ctx_id.GetId() < HTTP_MAX &&
	        m_http[http_ctx_id.GetId()].used);
}

bool Network::HttpValidTemplate(Id tmpl_id)
{
	Core::LockGuard lock(m_mutex);

	return (tmpl_id.GetType() == Id::Type::Template && m_templates.IndexValid(tmpl_id.GetId()) && m_templates.At(tmpl_id.GetId()).used);
}

bool Network::HttpValidConnection(Id conn_id)
{
	Core::LockGuard lock(m_mutex);

	return (conn_id.GetType() == Id::Type::Connection && m_connections.IndexValid(conn_id.GetId()) &&
	        m_connections.At(conn_id.GetId()).used);
}

bool Network::HttpValidRequest(Id req_id)
{
	Core::LockGuard lock(m_mutex);

	return (req_id.GetType() == Id::Type::Request && m_requests.IndexValid(req_id.GetId()) && m_requests.At(req_id.GetId()).used);
}

bool Network::HttpTerm(Id http_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id))
	{
		m_http[http_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver, bool is_auto_proxy_conf)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id))
	{
		HttpTemplate tn {};
		tn.used               = true;
		tn.http_ver           = http_ver;
		tn.user_agent         = String::FromUtf8(user_agent);
		tn.is_auto_proxy_conf = is_auto_proxy_conf;
		tn.http_ctx_id        = http_ctx_id.GetId();
		tn.nonblock           = false;

		int index = 0;
		for (auto& t: m_templates)
		{
			if (!t.used)
			{
				t = tn;
				return Id::Create(index, Id::Type::Template);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_templates.Add(tn);
			return Id::Create(index, Id::Type::Template);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id))
	{
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = String::FromUtf8(url);
		// cn.tmpl_id           = tmpl_id.ToInt();

		int index = 0;
		for (auto& t: m_connections)
		{
			if (!t.used)
			{
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_connections.Add(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteConnection(Id conn_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id))
	{
		m_connections[conn_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url, uint64_t content_length)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id))
	{
		HttpRequest cn(m_connections[conn_id.GetId()]);
		cn.used   = true;
		cn.method = String::FromUtf8(method);
		cn.url    = String::FromUtf8(url);
		// cn.conn_id        = conn_id.ToInt();
		cn.content_length = content_length;

		int index = 0;
		for (auto& t: m_requests)
		{
			if (!t.used)
			{
				t = cn;
				return Id::Create(index, Id::Type::Request);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_requests.Add(cn);
			return Id::Create(index, Id::Type::Request);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteRequest(Id req_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id))
	{
		m_requests[req_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpDeleteTemplate(Id tmpl_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id))
	{
		m_templates[tmpl_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpSetNonblock(Id id, bool enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->nonblock = enable;
		return true;
	}

	return false;
}

bool Network::HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->ssl_cbfunc   = cbfunc;
		base->ssl_user_arg = user_arg;
		return true;
	}

	return false;
}

bool Network::HttpsDisableOption(Id id, uint32_t ssl_flags)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->ssl_flags &= ~ssl_flags;
		return true;
	}

	return false;
}

bool Network::HttpAddRequestHeader(Id id, const char* name, const char* value, bool add)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		HttpHeader nh({String::FromUtf8(name), String::FromUtf8(value)});
		if (add)
		{
			base->headers.Add(nh);
		} else
		{
			for (auto& h: base->headers)
			{
				if (h.name == nh.name)
				{
					h.value = nh.value;
				}
			}
		}
		return true;
	}

	return false;
}

bool Network::HttpSetResolveTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	}

	if (base != nullptr)
	{
		base->resolve_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetResolveRetry(Id id, int32_t retry)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	}

	if (base != nullptr)
	{
		base->resolve_retry = retry;
		return true;
	}

	return false;
}

bool Network::HttpSetConnectTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->connect_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetSendTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->send_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetRecvTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->recv_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetAutoRedirect(Id id, int enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->auto_redirect = (enable != 0);
		return true;
	}

	return false;
}

bool Network::HttpSetAuthEnabled(Id id, int enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->auth_enabled = (enable != 0);
		return true;
	}

	return false;
}

namespace Net {

LIB_NAME("Net", "Net");

struct NetEtherAddr
{
	uint8_t data[6] = {0};
};

enum class GuestStringSnapshot
{
	Success,
	TooLong,
	Fault,
};

static GuestStringSnapshot SnapshotGuestString(const char* source, size_t capacity,
                                                std::string* snapshot)
{
	if (source == nullptr || snapshot == nullptr || capacity == 0) return GuestStringSnapshot::Fault;
	snapshot->clear();
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(source), capacity, snapshot, false))
		return GuestStringSnapshot::Fault;
	return snapshot->size() < capacity ? GuestStringSnapshot::Success : GuestStringSnapshot::TooLong;
#elif defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	const uint64_t source_address = reinterpret_cast<uint64_t>(source);
	if (source_address > UINT64_MAX - (capacity - 1)) return GuestStringSnapshot::Fault;
	for (size_t index = 0; index < capacity; ++index)
	{
		char byte = 0;
		if (!Loader::ReadGuestMemory(source_address + index, &byte, 1))
			return GuestStringSnapshot::Fault;
		if (byte == '\0') return GuestStringSnapshot::Success;
		snapshot->push_back(byte);
	}
	return GuestStringSnapshot::TooLong;
#else
	size_t length = 0;
	while (length < capacity && source[length] != '\0') ++length;
	snapshot->assign(source, length);
	return length < capacity ? GuestStringSnapshot::Success : GuestStringSnapshot::TooLong;
#endif
}

int LUMEN_SYSV_ABI NetInit()
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NetTerm()
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NetPoolCreate(const char* name, int size, int flags)
{
	PRINT_NAME();

	constexpr size_t NET_NAME_CAPACITY = 256;
	std::string name_snapshot;
	const auto name_status = SnapshotGuestString(name, NET_NAME_CAPACITY, &name_snapshot);
	if (name_status == GuestStringSnapshot::Fault) return NET_ERROR_EFAULT;
	if (name_status == GuestStringSnapshot::TooLong) return NET_ERROR_ENAMETOOLONG;
	if (flags != 0 || size <= 0) return NET_ERROR_EINVAL;
	if (g_net == nullptr) return NET_ERROR_ENOTINIT;

	name = name_snapshot.c_str();
	printf("\t name = %s\n", name);
	printf("\t size = %d\n", size);
	printf("\t flags = %d\n", flags);

	int id = g_net->PoolCreate(name, size);

	if (id < 0)
	{
		return NET_ERROR_ENFILE;
	}

	return id;
}

int LUMEN_SYSV_ABI NetPoolDestroy(int memid)
{
	PRINT_NAME();

	if (g_net == nullptr) return NET_ERROR_ENOTINIT;

	if (!g_net->PoolDestroy(memid))
	{
		return NET_ERROR_EBADF;
	}

	return OK;
}

// The common runtime deliberately has no host-network bridge. These entry
// points expose that capability boundary as ordinary guest errors and never
// inspect or modify guest buffers on the failure path.
int LUMEN_SYSV_ABI NetAccept(int, void*, uint32_t*) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetBind(int, const void*, uint32_t) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetConnect(int, const void*, uint32_t) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetEpollAbort(int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetEpollControl(int, int, int, void*) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetEpollCreate(const char*, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetEpollDestroy(int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetEpollWait(int, void*, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetGetSockInfo(int, void*, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetGetsockname(int, void*, uint32_t*) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetGetsockopt(int, int, int, void*, uint32_t*) { return NET_ERROR_ENETDOWN; }

uint16_t LUMEN_SYSV_ABI NetHtons(uint16_t host_value)
{
	return static_cast<uint16_t>((host_value << 8u) | (host_value >> 8u));
}

int LUMEN_SYSV_ABI NetListen(int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetRecv(int, void*, uint64_t, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetRecvfrom(int, void*, uint64_t, int, void*, uint32_t*) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetResolverCreate(const char*, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetResolverDestroy(int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetResolverGetError(int, int32_t*) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetResolverStartAton(int, const void*, char*, int, int, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetResolverStartNtoa(int, const char*, void*, int, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetSend(int, const void*, uint64_t, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetSendto(int, const void*, uint64_t, int, const void*, uint32_t) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetSetsockopt(int, int, int, const void*, uint32_t) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetSocket(const char*, int, int, int) { return NET_ERROR_ENETDOWN; }
int LUMEN_SYSV_ABI NetSocketClose(int) { return NET_ERROR_EBADF; }

int LUMEN_SYSV_ABI NetInetPton(int af, const char* src, void* dst)
{
	PRINT_NAME();

	if (af != 2) return NET_ERROR_EAFNOSUPPORT;
	if (src == nullptr || dst == nullptr) return NET_ERROR_EFAULT;
	constexpr size_t IPV4_TEXT_CAPACITY = 16;
	std::string source_snapshot;
	const auto source_status = SnapshotGuestString(src, IPV4_TEXT_CAPACITY, &source_snapshot);
	if (source_status == GuestStringSnapshot::Fault) return NET_ERROR_EFAULT;
	if (source_status == GuestStringSnapshot::TooLong) return 0;

	uint8_t result[4] {};
	const char* cursor = source_snapshot.c_str();
	for (size_t octet = 0; octet < 4; ++octet)
	{
		if (*cursor < '0' || *cursor > '9') return 0;
		uint32_t value = 0;
		size_t digits = 0;
		while (*cursor >= '0' && *cursor <= '9')
		{
			if (++digits > 3) return 0;
			value = value * 10u + static_cast<uint32_t>(*cursor - '0');
			if (value > 255u) return 0;
			++cursor;
		}
		result[octet] = static_cast<uint8_t>(value);
		if (octet != 3)
		{
			if (*cursor != '.') return 0;
			++cursor;
		} else if (*cursor != '\0')
		{
			return 0;
		}
	}

#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(dst), result, sizeof(result)))
		return NET_ERROR_EFAULT;
#else
	std::memcpy(dst, result, sizeof(result));
#endif
	return 1;
}

int LUMEN_SYSV_ABI NetEtherNtostr(const NetEtherAddr* n, char* str, size_t len)
{
	PRINT_NAME();

	NetEtherAddr zero {};

	EXIT_NOT_IMPLEMENTED(len != 18);
	EXIT_NOT_IMPLEMENTED(n == nullptr);
	EXIT_NOT_IMPLEMENTED(str == nullptr);
	EXIT_NOT_IMPLEMENTED(memcmp(n->data, zero.data, sizeof(zero.data)) != 0);

	strcpy(str, "00:00:00:00:00:00"); // NOLINT

	return OK;
}

int LUMEN_SYSV_ABI NetGetMacAddress(NetEtherAddr* addr, int flags)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);

	memset(addr->data, 0, sizeof(addr->data));

	return OK;
}

} // namespace Net

namespace Ssl {

LIB_NAME("Ssl", "Ssl");

int LUMEN_SYSV_ABI SslInit(uint64_t pool_size)
{
	PRINT_NAME();

	printf("\t size = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->SslInit(pool_size);

	if (!id.IsValid())
	{
		return SSL_ERROR_OUT_OF_SIZE;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI SslTerm(int ssl_ctx_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslTerm(Network::Id(ssl_ctx_id)))
	{
		return SSL_ERROR_INVALID_ID;
	}

	return OK;
}

} // namespace Ssl

namespace Http {

struct HttpEpoll
{
	Network::Id http_ctx_id = Network::Id(0);
	Network::Id request_id  = Network::Id(0);
	void*       user_arg    = nullptr;
};

LIB_NAME("Http", "Http");

int LUMEN_SYSV_ABI HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size)
{
	PRINT_NAME();

	printf("\t memid      = %d\n", memid);
	printf("\t ssl_ctx_id = %d\n", ssl_ctx_id);
	printf("\t size       = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->HttpInit(memid, Network::Id(ssl_ctx_id), pool_size);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI HttpTerm(int http_ctx_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpTerm(Network::Id(http_ctx_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver, int is_auto_proxy_conf)
{
	PRINT_NAME();

	printf("\t http_ctx_id        = %d\n", http_ctx_id);
	printf("\t user_agent         = %s\n", user_agent);
	printf("\t http_ver           = %d\n", http_ver);
	printf("\t is_auto_proxy_conf = %d\n", is_auto_proxy_conf);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateTemplate(Network::Id(http_ctx_id), user_agent, http_ver, is_auto_proxy_conf != 0);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI HttpDeleteTemplate(int tmpl_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteTemplate(Network::Id(tmpl_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetNonblock(int id, int enable)
{
	PRINT_NAME();

	printf("\t id     = %d\n", id);
	printf("\t enable = %d\n", enable);

	if (!g_net->HttpSetNonblock(Network::Id(id), enable != 0))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpsSetSslCallback(int id, HttpsCallback cbfunc, void* user_arg)
{
	PRINT_NAME();

	printf("\t id     = %d\n", id);

	if (!g_net->HttpsSetSslCallback(Network::Id(id), cbfunc, user_arg))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpsDisableOption(int id, uint32_t ssl_flags)
{
	PRINT_NAME();

	printf("\t id        = %d\n", id);
	printf("\t ssl_flags = %u\n", ssl_flags);

	if (!g_net->HttpsDisableOption(Network::Id(id), ssl_flags))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetResolveTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	printf("\t id   = %d\n", id);
	printf("\t usec = %u\n", usec);

	if (!g_net->HttpSetResolveTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetResolveRetry(int id, int32_t retry)
{
	PRINT_NAME();

	printf("\t id    = %d\n", id);
	printf("\t retry = %d\n", retry);

	if (!g_net->HttpSetResolveRetry(Network::Id(id), retry))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetConnectTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	printf("\t id   = %d\n", id);
	printf("\t usec = %u\n", usec);

	if (!g_net->HttpSetConnectTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetSendTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	printf("\t id   = %d\n", id);
	printf("\t usec = %u\n", usec);

	if (!g_net->HttpSetSendTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetRecvTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	printf("\t id   = %d\n", id);
	printf("\t usec = %u\n", usec);

	if (!g_net->HttpSetRecvTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetAutoRedirect(int id, int enable)
{
	PRINT_NAME();

	printf("\t id     = %d\n", id);
	printf("\t enable = %d\n", enable);

	if (!g_net->HttpSetAutoRedirect(Network::Id(id), enable))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpSetAuthEnabled(int id, int enable)
{
	PRINT_NAME();

	printf("\t id     = %d\n", id);
	printf("\t enable = %d\n", enable);

	if (!g_net->HttpSetAuthEnabled(Network::Id(id), enable))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode)
{
	PRINT_NAME();

	printf("\t id    = %d\n", id);
	printf("\t name  = %s\n", name);
	printf("\t value = %s\n", value);
	printf("\t mode  = %u\n", mode);

	EXIT_NOT_IMPLEMENTED(mode != 0 && mode != 1);

	if (!g_net->HttpAddRequestHeader(Network::Id(id), name, value, mode == 1))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpCreateEpoll(int http_ctx_id, HttpEpollHandle* eh)
{
	PRINT_NAME();

	printf("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	auto* epoll         = new HttpEpoll;
	epoll->http_ctx_id = Network::Id(http_ctx_id);
	g_net->TrackEpoll(epoll);
	*eh = epoll;

	return OK;
}

int LUMEN_SYSV_ABI HttpDestroyEpoll(int http_ctx_id, HttpEpollHandle eh)
{
	PRINT_NAME();

	printf("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	EXIT_NOT_IMPLEMENTED(!g_net->UntrackEpoll(eh));
	delete eh;

	return OK;
}

int LUMEN_SYSV_ABI HttpSetEpoll(int id, HttpEpollHandle eh, void* user_arg)
{
	PRINT_NAME();

	printf("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	eh->request_id = Network::Id(id);
	eh->user_arg   = user_arg;

	return OK;
}

int LUMEN_SYSV_ABI HttpUnsetEpoll(int id)
{
	PRINT_NAME();

	printf("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	return OK;
}

int LUMEN_SYSV_ABI HttpSendRequest(int request_id, const void* /*post_data*/, size_t /*size*/)
{
	PRINT_NAME();

	printf("\t request_id = %d\n", request_id);

	return HTTP_ERROR_TIMEOUT;
}

int LUMEN_SYSV_ABI HttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive)
{
	PRINT_NAME();

	printf("\t tmpl_id           = %d\n", tmpl_id);
	printf("\t url               = %s\n", url);
	printf("\t enable_keep_alive = %d\n", enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateConnectionWithURL(Network::Id(tmpl_id), url, enable_keep_alive != 0);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI HttpDeleteConnection(int conn_id)
{
	PRINT_NAME();

	printf("\t conn_id = %d\n", conn_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteConnection(Network::Id(conn_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int LUMEN_SYSV_ABI HttpCreateRequestWithURL2(int conn_id, const char* method, const char* url, uint64_t content_length)
{
	PRINT_NAME();

	printf("\t conn_id        = %d\n", conn_id);
	printf("\t url            = %s\n", url);
	printf("\t method         = %s\n", method);
	printf("\t content_length = %" PRIu64 "\n", content_length);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method, url, content_length);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI HttpDeleteRequest(int req_id)
{
	PRINT_NAME();

	printf("\t req_id = %d\n", req_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteRequest(Network::Id(req_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

} // namespace Http

void Network::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);

	for (auto* epoll: m_epolls)
	{
		delete epoll;
	}
	m_epolls.Clear();

	for (auto& pool: m_pools)
	{
		pool.used = false;
		pool.name.Clear();
		pool.size = 0;
	}
	for (auto& ssl: m_ssl)
	{
		ssl.used = false;
		ssl.size = 0;
	}
	for (auto& http: m_http)
	{
		http.used       = false;
		http.size       = 0;
		http.memid      = 0;
		http.ssl_ctx_id = 0;
	}
	m_templates.Clear();
	m_connections.Clear();
	m_requests.Clear();
	m_np_async_request_id = 0;
	m_np_web_api_id       = 0;
	NpRequests::ResetSessionState();
}

void Network::TrackEpoll(::Lumen::Libs::Network::Http::HttpEpoll* epoll)
{
	EXIT_IF(epoll == nullptr);
	Core::LockGuard lock(m_mutex);
	m_epolls.Add(epoll);
}

bool Network::UntrackEpoll(::Lumen::Libs::Network::Http::HttpEpoll* epoll)
{
	if (epoll == nullptr)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	return m_epolls.Remove(epoll);
}

int Network::CreateNpAsyncRequestId()
{
	Core::LockGuard lock(m_mutex);
	const int limit = NpRequests::TrackingEnabled() ? Id::MAX_ID : 1;
	if (m_np_async_request_id >= limit)
	{
		return -1;
	}
	const int id = ++m_np_async_request_id;
	NpRequests::RecordCreated(id);
	return id;
}

bool Network::HasIssuedNpAsyncRequestId(int id)
{
	Core::LockGuard lock(m_mutex);
	return id > 0 && id <= m_np_async_request_id;
}

int Network::CreateNpWebApiId()
{
	Core::LockGuard lock(m_mutex);
	if (m_np_web_api_id >= Id::MAX_ID)
	{
		return -1;
	}
	return ++m_np_web_api_id;
}

namespace NetCtl {

LIB_NAME("NetCtl", "NetCtl");

struct NetInAddr
{
	uint32_t s_addr = 0;
};

struct NetEtherAddr
{
	uint8_t data[6];
};

struct NetCtlNatInfo
{
	unsigned int size       = sizeof(NetCtlNatInfo);
	int          stunStatus = 0;
	int          natType    = 0;
	NetInAddr    mappedAddr;
};

union NetCtlInfo
{
	uint32_t     device;
	NetEtherAddr ether_addr;
	uint32_t     mtu;
	uint32_t     link;
	NetEtherAddr bssid;
	char         ssid[32 + 1];
	uint32_t     wifi_security;
	uint8_t      rssi_dbm;
	uint8_t      rssi_percentage;
	uint8_t      channel;
	uint32_t     ip_config;
	char         dhcp_hostname[255 + 1];
	char         pppoe_auth_name[127 + 1];
	char         ip_address[16];
	char         netmask[16];
	char         default_route[16];
	char         primary_dns[16];
	char         secondary_dns[16];
	uint32_t     http_proxy_config;
	char         http_proxy_server[255 + 1];
	uint16_t     http_proxy_port;
};

int LUMEN_SYSV_ABI NetCtlInit()
{
	PRINT_NAME();

	return OK;
}

void LUMEN_SYSV_ABI NetCtlTerm()
{
	PRINT_NAME();
}

int LUMEN_SYSV_ABI NetCtlGetNatInfo(NetCtlNatInfo* nat_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(nat_info == nullptr);
	EXIT_NOT_IMPLEMENTED(nat_info->size != sizeof(NetCtlNatInfo));

	nat_info->stunStatus        = 1;
	nat_info->natType           = 3;
	nat_info->mappedAddr.s_addr = 0x7f000001;

	return OK;
}

int LUMEN_SYSV_ABI NetCtlCheckCallback()
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NetCtlGetState(int* state)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	// Explicit local disconnected snapshot. This selects no network, without
	// authenticating NP or changing host connectivity. See NETCTL_STATE_INTERFACE.
	static const bool offline = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NETCTL_OFFLINE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	*state = offline ? 0 : 3;

	return OK;
}

int LUMEN_SYSV_ABI NetCtlRegisterCallback(NetCtlCallback func, void* /*arg*/, int* cid)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(func == nullptr);
	EXIT_NOT_IMPLEMENTED(cid == nullptr);

	*cid = 1;

	return OK;
}

int LUMEN_SYSV_ABI NetCtlGetInfo(int code, NetCtlInfo* info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(info == nullptr);

	printf("\t code = %d\n", code);

	switch (code)
	{
		case 2: memset(info->ether_addr.data, 0, sizeof(info->ether_addr.data)); break;
		case 11: info->ip_config = 0; break;
		case 14: strcpy(info->ip_address, "127.0.0.1"); break;
		default: EXIT("unknown code: %d\n", code);
	}

	return OK;
}

} // namespace NetCtl

namespace NpManager {

LIB_NAME("NpManager", "NpManager");

// The account-identifier profile records its own acceptances so a run can
// observe the call. The bound mirrors the other bounded traces of the runtime.
static constexpr int kAccountIdTraceLimit = 32;
static std::atomic<int> g_account_id_trace_count {0};

static bool IsSupportedRequestId(int id)
{
	// Keep the historical ID-1 behavior. Additional IDs are admitted only when
	// the experiment's allocator actually issued them in this Network session.
	return id == 1 || (NpRequests::TrackingEnabled() && g_net != nullptr && g_net->HasIssuedNpAsyncRequestId(id));
}

struct NpTitleId
{
	char    id[12 + 1];
	uint8_t padding[3];
};

struct NpTitleSecret
{
	uint8_t data[128];
};

struct NpCountryCode
{
	char data[2];
	char term;
	char padding[1];
};

struct NpAgeRestriction
{
	NpCountryCode country_code;
	int8_t        age;
	uint8_t       padding[3];
};

struct NpContentRestriction
{
	size_t                  size;
	int8_t                  default_age_restriction;
	char                    padding[3];
	int32_t                 age_restriction_count;
	const NpAgeRestriction* age_restriction;
};

struct NpOnlineId
{
	char data[16];
	char term;
	char dummy[3];
};

struct NpId
{
	NpOnlineId handle;
	uint8_t    opt[8];
	uint8_t    reserved[8];
};

// Eight-byte account-identifier output. The caller reached by a title reserves
// that width next to its own result field; the offline profile publishes no
// byte in it because the identifier belongs to a PlayStation Network sign-in
// and the emulated console has none.
struct NpAccountId
{
	uint64_t id;
};

static_assert(sizeof(int) == sizeof(int32_t));
static_assert(sizeof(NpOnlineId) == 20 && sizeof(NpId) == 36);
static_assert(offsetof(NpId, opt) == 20 && offsetof(NpId, reserved) == 28);
static_assert(sizeof(NpAccountId) == 8);

struct NpCreateAsyncRequestParameter
{
	size_t                   size;
	LibKernel::KernelCpumask cpu_affinity_mask;
	int                      thread_priority;
	uint8_t                  padding[4];
};

int LUMEN_SYSV_ABI NpCheckCallback()
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(title_id == nullptr);
	EXIT_NOT_IMPLEMENTED(title_secret == nullptr);

	printf("\t title_id = %.12s\n", title_id->id);
	printf("\t title_secret = %s\n", String::HexFromBin(Core::ByteBuffer(title_secret->data, 128)).C_Str());

	return OK;
}

int LUMEN_SYSV_ABI NpSetContentRestriction(const NpContentRestriction* restriction)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(restriction == nullptr);
	EXIT_NOT_IMPLEMENTED(restriction->size != sizeof(NpContentRestriction));

	printf("\t default_age_restriction = %" PRIi8 "\n", restriction->default_age_restriction);
	printf("\t age_restriction_count   = %" PRIi32 "\n", restriction->age_restriction_count);

	for (int i = 0; i < restriction->age_restriction_count; i++)
	{
		printf("\t age_restriction[%d].age = %" PRIi8 "\n", i, restriction->age_restriction[i].age);
		printf("\t age_restriction[%d].country_code.data = %.2s\n", i, restriction->age_restriction[i].country_code.data);
	}

	return OK;
}

int LUMEN_SYSV_ABI NpRegisterStateCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

void LUMEN_SYSV_ABI NpRegisterGamePresenceCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();
}

int LUMEN_SYSV_ABI NpRegisterPlusEventCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NpRegisterNpReachabilityStateCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NpGetNpId(int user_id, NpId* np_id)
{
	PRINT_NAME();

	printf("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(np_id == nullptr);

	// A local identity must not depend on the caller's previous buffer contents.
	NpId local {};
	std::memcpy(local.handle.data, "Lumen", sizeof("Lumen"));
	*np_id = local;

	return OK;
}

int LUMEN_SYSV_ABI NpGetOnlineId(int user_id, NpOnlineId* online_id)
{
	PRINT_NAME();

	printf("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(online_id == nullptr);

	NpOnlineId local {};
	std::memcpy(local.data, "Lumen", sizeof("Lumen"));
	*online_id = local;

	return OK;
}

int LUMEN_SYSV_ABI NpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);
	EXIT_NOT_IMPLEMENTED(param == nullptr);

	printf("\t size              = %" PRIu64 "\n", param->size);
	printf("\t cpu_affinity_mask = %" PRIu64 "\n", param->cpu_affinity_mask);
	printf("\t thread_priority   = %d\n", param->thread_priority);

	const int id = g_net->CreateNpAsyncRequestId();
	EXIT_NOT_IMPLEMENTED(id < 0);
	return id;
}

int LUMEN_SYSV_ABI NpAbortRequest(int req_id)
{
	PRINT_NAME();

	// Original quiescent-local-request policy, not a PSN cancellation service.
	// Historical issuance (including the legacy ID-1 exception) is not liveness.
	return NpRequests::QuiesceRequest(req_id) ? OK : NP_ERROR_INVALID_ARGUMENT;
}

int LUMEN_SYSV_ABI NpDeleteRequest(int req_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(!IsSupportedRequestId(req_id));

	printf("\t req_id = %d\n", req_id);
	NpRequests::RecordDeleted(req_id);

	return OK;
}

int LUMEN_SYSV_ABI NpCheckNpAvailability(int req_id, const char* user, void* result)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(!IsSupportedRequestId(req_id));
	EXIT_NOT_IMPLEMENTED(user == nullptr);
	EXIT_NOT_IMPLEMENTED(result != nullptr);

	printf("\t req_id = %d\n", req_id);
	printf("\t user   = %s\n", user);

	return OK;
}

int LUMEN_SYSV_ABI NpPollAsync(int req_id, int* result)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(!IsSupportedRequestId(req_id));
	EXIT_NOT_IMPLEMENTED(result == nullptr);

	printf("\t req_id = %d\n", req_id);

	*result = 0;

	return 0;
}

int LUMEN_SYSV_ABI NpGetState(int user_id, uint32_t* state)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	printf("\t user_id = %d\n", user_id);

	// Reviewed public states: Unknown=0, SignedOut=1, SignedIn=2.
	// Preserve the existing unknown snapshot; this is not PSN authentication.
	*state = 0;

	return OK;
}

int LUMEN_SYSV_ABI NpGetAccountIdA(int user_id, NpAccountId* account_id)
{
	PRINT_NAME();

	// Local offline profile. The account identifier is the identifier of a
	// PlayStation Network account and exists only for a signed-in user; the
	// emulated console has no PSN sign-in at all (the same offline model the
	// other NpManager queries belong to). Every admitted call is therefore
	// answered with the documented signed-out error and publishes no byte: the
	// profile never fabricates an account. A null output is rejected before
	// that answer, following the argument policy of the family. The console
	// result for a console with a local user but no PSN account is not
	// measured, and the profile does not depend on the user id: the emulated
	// sign-in state is a property of the console, not of the caller.
	if (account_id == nullptr) return NP_ERROR_INVALID_ARGUMENT;

	const int count = g_account_id_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kAccountIdTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_ACCOUNT_ID] call=%d user=%d result=signed-out model=offline-no-psn console_contract=unverified\n",
		             count + 1, user_id);
		std::fflush(stderr);
	}

	return NP_ERROR_SIGNED_OUT;
}

} // namespace NpManager

namespace NpCommon {

LIB_NAME("NpCommon", "NpCommon");

int LUMEN_SYSV_ABI NpCmpNpId(const NpManager::NpId* left, const NpManager::NpId* right)
{
	PRINT_NAME();

	if (left == nullptr || right == nullptr) return NP_ERROR_INVALID_ARGUMENT;

	// Local bounded C-string policy; the separate terminator and padding are
	// not identity fields. See NP_CMP_ABORT_INTERFACE.md for the evidence limits.
	for (size_t byte = 0; byte < sizeof(left->handle.data); ++byte)
	{
		if (left->handle.data[byte] != right->handle.data[byte]) return NP_ERROR_UTILITY_NOT_MATCH;
		if (left->handle.data[byte] == '\0') break;
	}
	return std::memcmp(left->opt, right->opt, sizeof(left->opt)) == 0 ? OK : NP_ERROR_UTILITY_NOT_MATCH;
}

} // namespace NpCommon

namespace NpManagerForToolkit {

LIB_NAME("NpManagerForToolkit", "NpManager");

int LUMEN_SYSV_ABI NpRegisterStateCallbackForToolkit(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI NpCheckCallbackForLib()
{
	PRINT_NAME();

	return OK;
}

} // namespace NpManagerForToolkit

namespace NpTrophy {

LIB_NAME("NpTrophy", "NpTrophy");

struct NpTrophyFlagArray
{
	uint32_t flag_bits[4];
};

struct NpTrophyGameDetails
{
	uint64_t size;
	uint32_t group_count;
	uint32_t trophy_count;
	uint32_t platinum_count;
	uint32_t gold_count;
	uint32_t silver_count;
	uint32_t bronze_count;
	char     title[128];
	char     description[1024];
};

struct NpTrophyGameData
{
	uint64_t size;
	uint32_t unlocked_count;
	uint32_t unlocked_platinum_count;
	uint32_t unlocked_gold_count;
	uint32_t unlocked_silver_count;
	uint32_t unlocked_bronze_count;
	uint32_t progress_percentage;
};

static_assert(sizeof(NpTrophyGameDetails) == 1184);
static_assert(offsetof(NpTrophyGameDetails, group_count) == 8);
static_assert(offsetof(NpTrophyGameDetails, trophy_count) == 12);
static_assert(offsetof(NpTrophyGameDetails, platinum_count) == 16);
static_assert(offsetof(NpTrophyGameDetails, gold_count) == 20);
static_assert(offsetof(NpTrophyGameDetails, silver_count) == 24);
static_assert(offsetof(NpTrophyGameDetails, bronze_count) == 28);
static_assert(offsetof(NpTrophyGameDetails, title) == 32);
static_assert(offsetof(NpTrophyGameDetails, description) == 160);
static_assert(sizeof(NpTrophyGameData) == 32);
static_assert(offsetof(NpTrophyGameData, unlocked_count) == 8);
static_assert(offsetof(NpTrophyGameData, unlocked_platinum_count) == 12);
static_assert(offsetof(NpTrophyGameData, unlocked_gold_count) == 16);
static_assert(offsetof(NpTrophyGameData, unlocked_silver_count) == 20);
static_assert(offsetof(NpTrophyGameData, unlocked_bronze_count) == 24);
static_assert(offsetof(NpTrophyGameData, progress_percentage) == 28);

namespace {

bool InvalidTrophyOutputRanges(const NpTrophyGameDetails* details, const NpTrophyGameData* data)
{
	constexpr uintptr_t max_address = std::numeric_limits<uintptr_t>::max();
	const uintptr_t details_address = reinterpret_cast<uintptr_t>(details);
	const uintptr_t data_address    = reinterpret_cast<uintptr_t>(data);
	if ((details != nullptr && sizeof(*details) > max_address - details_address) ||
	    (data != nullptr && sizeof(*data) > max_address - data_address))
		return true;
	if (details == nullptr || data == nullptr) return false;

	const uintptr_t details_end = details_address + sizeof(*details);
	const uintptr_t data_end    = data_address + sizeof(*data);
	return details_address < data_end && data_address < details_end;
}

} // namespace

int LUMEN_SYSV_ABI NpTrophyCreateHandle(int* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);

	*handle = 1;

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyCreateContext(int* context, int user_id, uint32_t service_label, uint64_t options)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(context == nullptr);
	EXIT_NOT_IMPLEMENTED(options != 0);

	*context = 1;

	printf("\t user_id       = %d\n", user_id);
	printf("\t service_label = %u\n", service_label);

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyRegisterContext(int context, int handle, uint64_t options)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(options != 0);
	EXIT_NOT_IMPLEMENTED(context != 1);
	EXIT_NOT_IMPLEMENTED(handle != 1);

	printf("\t context = %d\n", context);
	printf("\t handle  = %d\n", handle);

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyDestroyHandle(int handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);

	printf("\t handle  = %d\n", handle);

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyGetGameInfo(int context, int handle, NpTrophyGameDetails* details, NpTrophyGameData* data)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(context != 1);
	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(InvalidTrophyOutputRanges(details, data));

	// Original local empty profile: preserve the caller-owned size fields and
	// publish no fabricated Trophy metadata, entitlement or progress.
	if (details != nullptr)
		std::memset(reinterpret_cast<uint8_t*>(details) + sizeof(details->size), 0, sizeof(*details) - sizeof(details->size));
	if (data != nullptr)
		std::memset(reinterpret_cast<uint8_t*>(data) + sizeof(data->size), 0, sizeof(*data) - sizeof(data->size));

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyGetTrophyUnlockState(int context, int handle, NpTrophyFlagArray* flags, uint32_t* count)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(flags == nullptr);
	EXIT_NOT_IMPLEMENTED(count == nullptr);
	EXIT_NOT_IMPLEMENTED(context != 1);
	EXIT_NOT_IMPLEMENTED(handle != 1);

	printf("\t context = %d\n", context);
	printf("\t handle  = %d\n", handle);

	flags->flag_bits[0] = 0;
	flags->flag_bits[1] = 0;
	flags->flag_bits[2] = 0;
	flags->flag_bits[3] = 0;

	*count = 0;

	return OK;
}

int LUMEN_SYSV_ABI NpTrophyUnlockTrophy(int context, int handle, int trophy_id, int* platinum_id)
{
	PRINT_NAME();

	printf("\t context    = %d\n", context);
	printf("\t handle     = %d\n", handle);
	printf("\t trophy_id  = %d\n", trophy_id);

	if (platinum_id != nullptr)
	{
		*platinum_id = -1; // No platinum unlocked
	}

	return OK;
}

} // namespace NpTrophy

namespace NpWebApi {

LIB_NAME("NpWebApi", "NpWebApi");

int LUMEN_SYSV_ABI NpWebApiInitialize(int http_ctx_id, size_t pool_size)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	printf("\t http_ctx_id = %d\n", http_ctx_id);
	printf("\t pool_size   = %" PRIu64 "\n", pool_size);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	const int id = g_net->CreateNpWebApiId();
	EXIT_NOT_IMPLEMENTED(id < 0);
	return id;
}

int LUMEN_SYSV_ABI NpWebApiTerminate(int lib_ctx_id)
{
	PRINT_NAME();

	printf("\t lib_ctx_id = %d\n", lib_ctx_id);

	return OK;
}

} // namespace NpWebApi

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool NetworkSessionResetContractTest()
{
	Network network;

	const int pool = network.PoolCreate("session-one", 4096);
	const auto ssl = network.SslInit(4096);
	const auto http = network.HttpInit(pool, ssl, 4096);
	const auto tmpl = network.HttpCreateTemplate(http, "lumen", 1, false);
	const auto conn = network.HttpCreateConnectionWithURL(tmpl, "http://127.0.0.1", false);
	const auto request = network.HttpCreateRequestWithURL2(conn, "GET", "http://127.0.0.1", 0);
	if (pool != 0 || !ssl.IsValid() || !http.IsValid() || !tmpl.IsValid() || !conn.IsValid() || !request.IsValid() ||
	    network.CreateNpAsyncRequestId() != 1 || network.CreateNpWebApiId() != 1)
	{
		return false;
	}

	auto* epoll = new Http::HttpEpoll;
	network.TrackEpoll(epoll);
	network.ResetForNextSession();

	if (network.HttpValid(http) || network.HttpValidTemplate(tmpl) || network.HttpValidConnection(conn) ||
	    network.HttpValidRequest(request))
	{
		return false;
	}

	const int pool_two = network.PoolCreate("session-two", 8192);
	const auto ssl_two = network.SslInit(8192);
	const auto http_two = network.HttpInit(pool_two, ssl_two, 8192);
	const bool reset = pool_two == pool && ssl_two.ToInt() == ssl.ToInt() && http_two.ToInt() == http.ToInt() &&
	                   network.CreateNpAsyncRequestId() == 1 && network.CreateNpWebApiId() == 1;
	network.ResetForNextSession();
	return reset;
}
#endif

} // namespace Lumen::Libs::Network

#endif // LUMEN_EMU_ENABLED
