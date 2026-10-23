#ifndef LUMEN_EMULATOR_LIBS_NP_TOOLKIT2_H_
#define LUMEN_EMULATOR_LIBS_NP_TOOLKIT2_H_

#include "Emulator/Common.h"

#include <cstdint>

namespace Lumen::Libs::NpToolkit2 {

// Local profile for the NP Toolkit2 C++ request-object constructor reached by
// guest plugins (UnityNpToolkit2.prx). The console body belongs to the absent
// libSceNpToolkit2 image and its default object state is neither public nor
// measured on console, so the call is accepted without touching guest memory
// and every dependent service call keeps failing closed at its own call site.
void LUMEN_SYSV_ABI CoreRequestInitParamsConstruct(void* self);

// Local profile for sce::Toolkit::NP::V2::Core::init(const Request::InitParams&),
// called once by the same plugins with the address of the request object built
// above. The console body belongs to the absent libSceNpToolkit2 image, so the
// call is accepted without reading the object or creating library state: the
// caller only receives the accepted result and the first operation that
// actually depends on console library state keeps failing closed at its own
// symbol. The console result value is not measured.
int32_t LUMEN_SYSV_ABI CoreInit(const void* init_params);

// Local profile for sce::Toolkit::NP::V2::Core::Request::InitParams::~InitParams()
// (complete-object destructor variant D1). The accepted constructor writes no
// state, so the destructor has nothing to release; the call is accepted
// without touching guest memory.
void LUMEN_SYSV_ABI CoreRequestInitParamsDestroy(void* self);

// Local profile for sce::Toolkit::NP::V2::Core::Response<Core::Empty>::Response()
// (complete-object constructor variant C1). The caller owns the object and the
// console body belongs to the absent libSceNpToolkit2 image: neither the object
// layout nor the state the constructor would publish is public or measured on
// console, so the call is accepted without touching guest memory. Every
// operation that depends on the response state keeps failing closed at its own
// symbol; `get()` and the typed response accessors stay unregistered.
void LUMEN_SYSV_ABI CoreResponseEmptyConstruct(void* self);

// Local profile for Core::Response<Core::Empty>::~Response() (complete-object
// destructor variant D1). The accepted constructor writes no state, so the
// destructor has nothing to release; the call is accepted without touching
// guest memory.
void LUMEN_SYSV_ABI CoreResponseEmptyDestroy(void* self);

// Local profile for sce::Toolkit::NP::V2::Trophy::Request::RegisterTrophyPack::RegisterTrophyPack()
// (complete-object constructor variant C1), the first request object of the
// trophy family reached by a guest. As with the other request objects, the
// caller-owned memory is left untouched: the calling module prepares the fields
// it consumes and the first dependent service call keeps failing closed at its
// own symbol.
void LUMEN_SYSV_ABI TrophyRegisterTrophyPackRequestConstruct(void* self);

// Local profile for Trophy::Request::RegisterTrophyPack::~RegisterTrophyPack()
// (complete-object destructor variant D1). The accepted constructor writes no
// state, so the destructor has nothing to release; the call is accepted
// without touching guest memory.
void LUMEN_SYSV_ABI TrophyRegisterTrophyPackRequestDestroy(void* self);

// Local profile for sce::Toolkit::NP::V2::Trophy::registerTrophyPack(const
// Trophy::Request::RegisterTrophyPack&, Core::Response<Core::Empty>*), the
// first service call of the trophy family. The console body belongs to the
// absent libSceNpToolkit2 image: neither the request fields it would read nor
// the response state it would publish are public or measured on console, so
// the call is accepted without touching guest memory and returns the local
// accepted code 0. The observed call site treats a negative result as an
// error and every non-negative result as success; the console result value is
// not measured. Every operation that depends on console response state keeps
// failing closed at its own symbol.
int32_t LUMEN_SYSV_ABI TrophyRegisterTrophyPack(const void* request, void* response);

// Local profile for sce::Toolkit::NP::V2::Core::RequestBase::getFunctionType()
// const, the first operation that would read request-object state published by
// the absent libSceNpToolkit2 constructor. The emulated request objects carry
// no function type: the accessor accepts the call without reading or writing
// guest memory and returns the local unpublished marker 0, which lies outside
// the function-type range observed at the only dispatching consumer (1..0x79),
// so a consumer that dispatches on it takes its explicit unknown-type path
// instead of a wrong but plausible route. The marker is stable for the object,
// which is what the observed register/match call sites require; the console
// value is not measured and the toolkit completion dispatch stays unqualified.
int32_t LUMEN_SYSV_ABI CoreRequestBaseGetFunctionType(const void* self);

} // namespace Lumen::Libs::NpToolkit2

#endif // LUMEN_EMULATOR_LIBS_NP_TOOLKIT2_H_
