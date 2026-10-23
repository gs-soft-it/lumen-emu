#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

struct VaContext;
struct VaList;

using libc_printf_std_func_t   = LUMEN_FORMAT_PRINTF(1, 2) LUMEN_SYSV_ABI int (*)(const char* str, ...);
using libc_printf_ctx_func_t   = int (*)(VaContext* c);
using libc_snprintf_ctx_func_t = int (*)(VaContext* c);
using libc_sprintf_ctx_func_t  = int (*)(VaContext* c);
using libc_vprintf_func_t      = int (*)(const char* str, VaList* c);
using libc_vsnprintf_func_t    = int (*)(char* str, size_t size, const char* fmt, VaList* c);
using libc_vsprintf_func_t     = int (*)(char* str, const char* fmt, VaList* c);

libc_printf_std_func_t   GetPrintfStdFunc();
libc_printf_ctx_func_t   GetPrintfCtxFunc();
libc_snprintf_ctx_func_t GetSnrintfCtxFunc();
libc_sprintf_ctx_func_t  GetSprintfCtxFunc();
libc_vprintf_func_t      GetVprintfFunc();
libc_vsnprintf_func_t    GetVsnprintfFunc();
libc_vsprintf_func_t     GetVsprintfFunc();

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_PRINTF_H_ */
