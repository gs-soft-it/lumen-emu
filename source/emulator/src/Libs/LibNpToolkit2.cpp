#include "Emulator/Libs/NpToolkit2.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

// The loader normalizes the "libSce" prefix, so this identity matches the
// plugin import table exactly: library NpToolkit2 v1, module NpToolkit2 v0.0.
LIB_VERSION("libSceNpToolkit2", 1, "libSceNpToolkit2", 0, 0);

namespace NpToolkit2 {
namespace {

// The acceptance is recorded only to make it observable in a run; the guest
// pointers are formatted immediately and never retained. The bound mirrors the
// other bounded diagnostic traces of the runtime.
constexpr int kTraceLimit = 32;
std::atomic<int> g_construct_trace_count {0};
std::atomic<int> g_core_init_trace_count {0};
std::atomic<int> g_destroy_trace_count {0};
std::atomic<int> g_response_trace_count {0};
std::atomic<int> g_response_destroy_trace_count {0};
std::atomic<int> g_trophy_request_trace_count {0};
std::atomic<int> g_trophy_request_destroy_trace_count {0};
std::atomic<int> g_trophy_register_trace_count {0};
std::atomic<int> g_request_function_type_trace_count {0};

// The accepted response and request-object profiles record their call so it
// stays observable in a run; the guest pointer is formatted immediately and
// never retained.
void RecordAcceptedObjectCall(std::atomic<int>& counter, const char* tag, const void* self)
{
	const int count = counter.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "%s call=%d this=0x%016" PRIx64
		             " model=accepted-no-state reads=0 writes=0 console_contract=unverified\n",
		             tag, count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)));
		std::fflush(stderr);
	}
}

} // namespace

void LUMEN_SYSV_ABI CoreRequestInitParamsConstruct(void* self)
{
	// The console body belongs to the absent libSceNpToolkit2 image and the
	// default object state is neither public nor measured on console. Accept
	// the call without touching guest memory instead of inventing a layout; the
	// first dependent service call keeps failing closed on its own symbol.
	const int count = g_construct_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_TOOLKIT2_INIT_PARAMS] call=%d this=0x%016" PRIx64
		             " model=accepted-no-state writes=0 console_contract=unverified\n",
		             count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)));
		std::fflush(stderr);
	}
}

int32_t LUMEN_SYSV_ABI CoreInit(const void* init_params)
{
	// The console body belongs to the absent libSceNpToolkit2 image: the
	// request layout and the state the console initializer creates are neither
	// public nor measured. Accept the call without reading the object or
	// creating library state so the plugin's initialization sequence
	// continues; the first operation that depends on console library state
	// keeps failing closed on its own symbol.
	const int count = g_core_init_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_TOOLKIT2_CORE_INIT] call=%d params=0x%016" PRIx64
		             " model=accepted-no-state reads=0 writes=0 result=0 console_contract=unverified\n",
		             count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(init_params)));
		std::fflush(stderr);
	}
	return 0;
}

void LUMEN_SYSV_ABI CoreRequestInitParamsDestroy(void* self)
{
	// The accepted constructor writes no state, so the complete-object
	// destructor has nothing to release; accept the call without reading or
	// writing guest memory.
	const int count = g_destroy_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_TOOLKIT2_INIT_PARAMS_DESTROY] call=%d this=0x%016" PRIx64
		             " model=accepted-no-state reads=0 writes=0 console_contract=unverified\n",
		             count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)));
		std::fflush(stderr);
	}
}

void LUMEN_SYSV_ABI CoreResponseEmptyConstruct(void* self)
{
	// The console body belongs to the absent libSceNpToolkit2 image: the
	// response layout and the state `Response()` would publish are neither
	// public nor measured. Accept the construction of the caller-owned object
	// without touching guest memory; the first operation that depends on the
	// response state keeps failing closed on its own symbol.
	RecordAcceptedObjectCall(g_response_trace_count, "[NP_TOOLKIT2_RESPONSE_EMPTY]", self);
}

void LUMEN_SYSV_ABI CoreResponseEmptyDestroy(void* self)
{
	// The accepted constructor writes no state, so the complete-object
	// destructor has nothing to release; accept the call without reading or
	// writing guest memory.
	RecordAcceptedObjectCall(g_response_destroy_trace_count, "[NP_TOOLKIT2_RESPONSE_EMPTY_DESTROY]", self);
}

void LUMEN_SYSV_ABI TrophyRegisterTrophyPackRequestConstruct(void* self)
{
	// First request object of the trophy family. The calling module prepares
	// the fields it consumes and the console body belongs to the absent
	// libSceNpToolkit2 image, whose request layout is neither public nor
	// measured: accept the construction without touching guest memory.
	RecordAcceptedObjectCall(g_trophy_request_trace_count, "[NP_TOOLKIT2_TROPHY_REQUEST]", self);
}

void LUMEN_SYSV_ABI TrophyRegisterTrophyPackRequestDestroy(void* self)
{
	// The accepted constructor writes no state, so the complete-object
	// destructor has nothing to release; accept the call without reading or
	// writing guest memory.
	RecordAcceptedObjectCall(g_trophy_request_destroy_trace_count, "[NP_TOOLKIT2_TROPHY_REQUEST_DESTROY]", self);
}

int32_t LUMEN_SYSV_ABI TrophyRegisterTrophyPack(const void* request, void* response)
{
	// First service call of the trophy family. The console body belongs to the
	// absent libSceNpToolkit2 image: the request fields it would read and the
	// response state it would publish are neither public nor measured on
	// console. Accept the call without touching guest memory and report the
	// local accepted code: the observed call site treats a negative result as
	// an error and every non-negative result as success, and the console result
	// value is not measured. The first operation that depends on console
	// response state keeps failing closed on its own symbol.
	const int count = g_trophy_register_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_TOOLKIT2_TROPHY_REGISTER] call=%d request=0x%016" PRIx64 " response=0x%016" PRIx64
		             " model=accepted-no-state reads=0 writes=0 result=0 console_contract=unverified\n",
		             count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(request)),
		             static_cast<uint64_t>(reinterpret_cast<uintptr_t>(response)));
		std::fflush(stderr);
	}
	return 0;
}

int32_t LUMEN_SYSV_ABI CoreRequestBaseGetFunctionType(const void* self)
{
	// First operation of the trophy path that would read request-object state:
	// the console body belongs to the absent libSceNpToolkit2 image and the
	// function type is a field the console constructor of the request would
	// have published, which the accepted no-state object profiles do not
	// reproduce. Accept the call without touching guest memory and return the
	// local unpublished marker 0: it is stable for the object, which is what
	// the observed register/match call sites require (they key their pending
	// table on `(code, functionType)` of the same object), and it lies outside
	// the function-type range 1..0x79 observed at the only dispatching consumer
	// (`ft - 1` indexes a 0x79-entry jump table), so a consumer that dispatches
	// on it takes its explicit unknown-type path instead of a wrong but
	// plausible route. The console value is not measured.
	const int count = g_request_function_type_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[NP_TOOLKIT2_REQUEST_FUNCTION_TYPE] call=%d this=0x%016" PRIx64
		             " model=accepted-no-state reads=0 writes=0 result=0 marker=unpublished"
		             " console_contract=unverified\n",
		             count + 1, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)));
		std::fflush(stderr);
	}
	return 0;
}

} // namespace NpToolkit2

LIB_DEFINE(InitNpToolkit2_1)
{
	// vbh-MWtjMh4 = sce::Toolkit::NP::V2::Core::Request::InitParams::InitParams()
	// LLdnqVnnNBQ = sce::Toolkit::NP::V2::Core::init(const Core::Request::InitParams&)
	// -bAjEe-hdCg = sce::Toolkit::NP::V2::Core::Request::InitParams::~InitParams()
	// (complete-object variant D1). These identities are the request-object
	// lifecycle reached by UnityNpToolkit2.prx during its plugin initialization.
	// tlpXm5KLgqk = Core::Response<Core::Empty>::Response() (C1) and
	// 30Zxe35LtBg = Core::Response<Core::Empty>::~Response() (D1) are the
	// response object the same plugin constructs on the heap for commands that
	// carry no payload; wkgY4lDEV9k = Trophy::Request::RegisterTrophyPack::
	// RegisterTrophyPack() (C1) and ItZXngg8tfo = the matching D1 are the first
	// request object of the trophy family. buMmdpEwRuM = Trophy::
	// registerTrophyPack(const Request::RegisterTrophyPack&, Core::Response<
	// Core::Empty>*) is the matching service call: accepted/no-state with the
	// local accepted code 0. El4tFb9f8dk = Core::RequestBase::getFunctionType()
	// const is the first operation of the success path that would read
	// request-object state the console request constructor never published
	// here: accepted/no-state with the local unpublished marker 0. Every
	// profile accepts its call without reading or writing guest memory; the
	// remaining library imports stay unregistered and fail closed at their own
	// symbol.
	LIB_FUNC_SCALAR("vbh-MWtjMh4", NpToolkit2::CoreRequestInitParamsConstruct);
	LIB_FUNC_SCALAR("LLdnqVnnNBQ", NpToolkit2::CoreInit);
	LIB_FUNC_SCALAR("-bAjEe-hdCg", NpToolkit2::CoreRequestInitParamsDestroy);
	LIB_FUNC_SCALAR("tlpXm5KLgqk", NpToolkit2::CoreResponseEmptyConstruct);
	LIB_FUNC_SCALAR("30Zxe35LtBg", NpToolkit2::CoreResponseEmptyDestroy);
	LIB_FUNC_SCALAR("wkgY4lDEV9k", NpToolkit2::TrophyRegisterTrophyPackRequestConstruct);
	LIB_FUNC_SCALAR("ItZXngg8tfo", NpToolkit2::TrophyRegisterTrophyPackRequestDestroy);
	LIB_FUNC_SCALAR("buMmdpEwRuM", NpToolkit2::TrophyRegisterTrophyPack);
	LIB_FUNC_SCALAR("El4tFb9f8dk", NpToolkit2::CoreRequestBaseGetFunctionType);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
