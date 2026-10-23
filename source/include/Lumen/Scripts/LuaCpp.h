#ifndef INCLUDE_LUMEN_SCRIPTS_LUACPP_H_
#define INCLUDE_LUMEN_SCRIPTS_LUACPP_H_

#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
//#define LUA_BUILD_AS_DLL
//#define LUA_USE_WINDOWS
#else
#define LUA_USE_LINUX
#endif

// IWYU pragma: begin_exports
extern "C" {
#include "lua.h"
//
#include "lualib.h"
//
#include "lauxlib.h"
//
}
// IWYU pragma: end_exports

#endif /* INCLUDE_LUMEN_SCRIPTS_LUACPP_H_ */
