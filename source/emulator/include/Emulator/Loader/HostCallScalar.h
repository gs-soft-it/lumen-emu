#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_HOSTCALLSCALAR_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_HOSTCALLSCALAR_H_

#include "Emulator/Loader/HostCall.h"

#include <array>
#include <cstring>
#include <limits>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader {

// This metadata describes C++ scalar signatures. Guest register and stack
// placement belongs to the marshaler, independently of the native host ABI.
enum class HostCallScalarType : uint8_t
{
	Void,
	Gp,
	Float32,
	Float64,
	Unsupported,
};

inline constexpr uint32_t HOST_CALL_SCALAR_MAX_ARITY = 16;
static_assert(sizeof(float) == 4 && sizeof(double) == 8);
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
using HostCallScalarInvoker = uint64_t (*)(const uint64_t* arguments);

struct HostCallScalarMetadata
{
	uint32_t arity = 0;
	std::array<HostCallScalarType, HOST_CALL_SCALAR_MAX_ARITY> arguments {};
	HostCallScalarType result = HostCallScalarType::Void;
	HostCallScalarInvoker invoker = nullptr;
};

namespace HostCallScalarDetail {

template <typename T>
constexpr HostCallScalarType Type()
{
	using Value = std::remove_cv_t<T>;
	if constexpr (std::is_void_v<Value>) return HostCallScalarType::Void;
	else if constexpr (HostCallDetail::IsGpValue<Value>()) return HostCallScalarType::Gp;
	else if constexpr (std::is_same_v<Value, float>) return HostCallScalarType::Float32;
	else if constexpr (std::is_same_v<Value, double>) return HostCallScalarType::Float64;
	else return HostCallScalarType::Unsupported;
}

template <typename T>
T FromWord(uint64_t word)
{
	if constexpr (HostCallDetail::IsGpValue<T>())
	{
		return HostCallDetail::WordToGpValue<T>(word);
	} else
	{
		static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
		static_assert(sizeof(float) == 4 && sizeof(double) == 8);
		T value;
		if constexpr (std::is_same_v<T, float>)
		{
			const uint32_t bits = static_cast<uint32_t>(word);
			std::memcpy(&value, &bits, sizeof(value));
		} else
		{
			std::memcpy(&value, &word, sizeof(value));
		}
		return value;
	}
}

template <typename T>
uint64_t ToWord(T value)
{
	if constexpr (HostCallDetail::IsGpValue<T>())
	{
		return HostCallDetail::GpReturnToWord(value);
	} else
	{
		static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
		if constexpr (std::is_same_v<T, float>)
		{
			uint32_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		} else
		{
			uint64_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}
	}
}

template <typename Return, typename... Args>
constexpr bool SignatureSupported(LUMEN_SYSV_ABI Return (*)(Args...))
{
	return sizeof...(Args) <= HOST_CALL_SCALAR_MAX_ARITY &&
	       Type<Return>() != HostCallScalarType::Unsupported &&
	       ((Type<Args>() != HostCallScalarType::Unsupported && Type<Args>() != HostCallScalarType::Void) && ...);
}

template <typename Return, typename... Args>
constexpr bool SignatureSupported(LUMEN_SYSV_ABI Return (*)(Args..., ...))
{
	return false;
}

template <typename Return, typename... Args, size_t... Index>
uint64_t Invoke(LUMEN_SYSV_ABI Return (*function)(Args...), const uint64_t* arguments, std::index_sequence<Index...>)
{
	if constexpr (std::is_void_v<Return>)
	{
		function(FromWord<Args>(arguments[Index])...);
		return 0;
	} else
	{
		return ToWord(function(FromWord<Args>(arguments[Index])...));
	}
}

template <auto Function>
uint64_t InvokeBound(const uint64_t* arguments)
{
	return Invoke(Function, arguments, std::make_index_sequence<HostCallDetail::SignatureArity(Function)> {});
}

template <typename Return, typename... Args>
constexpr HostCallScalarMetadata Describe(LUMEN_SYSV_ABI Return (*)(Args...))
{
	return {sizeof...(Args), {Type<Args>()...}, Type<Return>(), nullptr};
}

} // namespace HostCallScalarDetail

template <auto Function>
constexpr HostCallScalarMetadata MakeHostCallScalarMetadata()
{
	static_assert(HostCallScalarDetail::SignatureSupported(Function),
	              "scalar host calls require at most 16 fixed integral, enum, pointer, float or double arguments");
	auto metadata = HostCallScalarDetail::Describe(Function);
	metadata.invoker = &HostCallScalarDetail::InvokeBound<Function>;
	return metadata;
}

} // namespace Lumen::Loader

#endif
#endif
