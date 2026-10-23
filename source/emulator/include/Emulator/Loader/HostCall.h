#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_HOSTCALL_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_HOSTCALL_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Cpu {
struct X64State;
} // namespace Lumen::Cpu

namespace Lumen::Loader {

using HostCallGpInvoker = uint64_t (*)(const uint64_t* arguments);

inline constexpr uint32_t HOST_CALL_GP_REGISTER_ARGUMENT_COUNT = 6;
inline constexpr uint32_t HOST_CALL_GP_LEGACY_ARITY             = 8;
inline constexpr uint32_t HOST_CALL_GP_MAX_ARITY                = 12;

static_assert(HOST_CALL_GP_REGISTER_ARGUMENT_COUNT < HOST_CALL_GP_LEGACY_ARITY);
static_assert(HOST_CALL_GP_LEGACY_ARITY <= HOST_CALL_GP_MAX_ARITY);

struct HostCallGpMetadata
{
	uint32_t          arity   = HOST_CALL_GP_LEGACY_ARITY;
	HostCallGpInvoker invoker = nullptr;
};

class SymbolDatabase;
#if defined(__aarch64__)
bool IsNativeHostCallTarget(uint64_t target);
#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
void     ResetNativeHostDladdrCallCountForTest();
uint64_t NativeHostDladdrCallCountForTest();
#endif
#endif
bool InvokeHostCall(const SymbolDatabase* symbols, uint64_t target, Cpu::X64State* state);

namespace HostCallDetail {

template <typename T>
constexpr bool IsGpValue()
{
	using Value = std::remove_cv_t<T>;
	if constexpr (std::is_void_v<Value>)
	{
		return false;
	} else
	{
		return (std::is_integral_v<Value> || std::is_enum_v<Value> || std::is_pointer_v<Value>) &&
		       sizeof(Value) <= sizeof(uint64_t);
	}
}

template <typename T>
T WordToGpValue(uint64_t word)
{
	using Value = std::remove_cv_t<T>;
	static_assert(IsGpValue<T>(), "host-call arguments must be integral, enum, or pointer values");
	static_assert(sizeof(Value) <= sizeof(uint64_t), "host-call arguments must fit in one guest qword");
	if constexpr (std::is_pointer_v<Value>)
	{
		return reinterpret_cast<Value>(static_cast<uintptr_t>(word));
	} else if constexpr (std::is_enum_v<Value>)
	{
		using Underlying = std::underlying_type_t<Value>;
		return static_cast<Value>(static_cast<Underlying>(word));
	} else
	{
		return static_cast<Value>(word);
	}
}

template <typename T>
uint64_t GpReturnToWord(T value)
{
	using Value = std::remove_cv_t<T>;
	static_assert(IsGpValue<T>(), "host-call returns must be void, integral, enum, or pointer values");
	static_assert(sizeof(Value) <= sizeof(uint64_t), "host-call returns must fit in one guest qword");
	if constexpr (std::is_pointer_v<Value>)
	{
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
	} else if constexpr (std::is_enum_v<Value>)
	{
		using Underlying = std::underlying_type_t<Value>;
		if constexpr (std::is_same_v<Underlying, bool>)
		{
			return static_cast<Underlying>(value) ? 1 : 0;
		} else
		{
			using Unsigned = std::make_unsigned_t<Underlying>;
			return static_cast<uint64_t>(static_cast<Unsigned>(static_cast<Underlying>(value)));
		}
	} else if constexpr (std::is_same_v<Value, bool>)
	{
		return value ? 1 : 0;
	} else
	{
		using Unsigned = std::make_unsigned_t<Value>;
		return static_cast<uint64_t>(static_cast<Unsigned>(value));
	}
}

template <typename Return, typename... Args>
constexpr uint32_t SignatureArity(LUMEN_SYSV_ABI Return (*)(Args...))
{
	return sizeof...(Args);
}

template <typename Return, typename... Args>
constexpr uint32_t SignatureArity(LUMEN_SYSV_ABI Return (*)(Args..., ...))
{
	return sizeof...(Args);
}

template <typename Return, typename... Args>
constexpr bool SignatureSupported(LUMEN_SYSV_ABI Return (*)(Args...))
{
	return (std::is_void_v<Return> || IsGpValue<Return>()) && (IsGpValue<Args>() && ...);
}

template <typename Return, typename... Args>
constexpr bool SignatureSupported(LUMEN_SYSV_ABI Return (*)(Args..., ...))
{
	return false;
}

template <typename Return, typename... Args, size_t... Index>
uint64_t Invoke(LUMEN_SYSV_ABI Return (*function)(Args...), const uint64_t* arguments, std::index_sequence<Index...> /*indices*/)
{
	if constexpr (std::is_void_v<Return>)
	{
		function(WordToGpValue<Args>(arguments[Index])...);
		return 0;
	} else
	{
		return GpReturnToWord(function(WordToGpValue<Args>(arguments[Index])...));
	}
}

template <auto Function>
uint64_t InvokeBound(const uint64_t* arguments)
{
	constexpr uint32_t arity = SignatureArity(Function);
	return Invoke(Function, arguments, std::make_index_sequence<arity> {});
}

} // namespace HostCallDetail

template <auto Function>
constexpr HostCallGpMetadata MakeHostCallGpMetadata()
{
	constexpr uint32_t arity = HostCallDetail::SignatureArity(Function);
	static_assert(HostCallDetail::SignatureSupported(Function),
	              "typed host calls support only non-variadic integral, enum, and pointer GP signatures");
	static_assert(arity > HOST_CALL_GP_LEGACY_ARITY && arity <= HOST_CALL_GP_MAX_ARITY,
	              "typed host-call metadata is reserved for fixed GP signatures above the legacy bridge arity");
	return {arity, &HostCallDetail::InvokeBound<Function>};
}

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_HOSTCALL_H_ */
