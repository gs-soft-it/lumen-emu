// This compile-only object intentionally includes the quarantine-pinned
// upstream public headers. It is never linked into the APK and contains no
// FEXCore context.
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <cstdint>
#include <type_traits>

namespace {

using FexX86Register = FEXCore::X86State::X86Reg;
using FexThunkApi = FEXCore::IR::ThunkedFunction;

static_assert(std::is_same_v<std::underlying_type_t<FexX86Register>,
                             std::uint32_t>);
static_assert(FEXCore::X86State::REG_RAX == 0);
static_assert(FEXCore::X86State::REG_R15 == 15);
static_assert(FEXCore::X86State::REG_INVALID == 255);
static_assert(std::is_function_v<FexThunkApi>);
static_assert(std::is_trivially_copyable_v<FEXCore::IR::SHA256Sum>);
static_assert(sizeof(FEXCore::IR::SHA256Sum) == 32);
static_assert(FEXCore::Utils::FEX_PAGE_SIZE == 4096);
static_assert(FEXCore::Utils::FEX_PAGE_SHIFT == 12);
#if !defined(LUMEN_PS5_FEX_C13)
static_assert(FEXCore::Utils::FEX_HOST_PAGE_ALIGNMENT == 16384);
#endif

[[gnu::used]] constexpr char kFexCoreHeaderProbeReceipt[] =
    "LUMEN_ANDROID_FEXCORE_HEADER_PROBE_V1 "
#if defined(LUMEN_PS5_FEX_C13)
    "pin=f2b679f6028ce1c38875233aecfcf5d3f8ebecec "
#else
    "pin=e17fdf9e90a49614e16ac8414a687477fb9e4f7c "
#endif
    "license=MIT headers=X86Enums+IR+TypeDefines "
    "compileOnly=1 linked=0 instantiated=0 executed=0";

} // namespace
