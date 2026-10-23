#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {

using namespace Lumen::Loader;
using ScalarType = HostCallScalarType;

// These are ordinary native C++ calls. The addresses used by the database are
// identities only: this test never publishes a host target or runs guest code.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "HostCallScalarMetadataTest: FAIL: %s\n", message);
	return condition;
}

template <typename Value, typename Bits>
Value ValueFromBits(Bits bits)
{
	static_assert(sizeof(Value) == sizeof(Bits));
	Value value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

template <typename Bits, typename Value>
Bits BitsFromValue(Value value)
{
	static_assert(sizeof(Value) == sizeof(Bits));
	Bits bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

enum class BoolValue: bool { Clear = false, Set = true };
enum class SignedValue: int16_t { Negative = -11, Positive = 9 };
struct Aggregate { uint64_t first; uint64_t second; };

uint32_t g_float_input = 0;
uint32_t g_float_output = 0;
uint64_t g_double_input = 0;
uint64_t g_double_output = 0;
unsigned g_void_calls = 0;
std::array<uint64_t, 6> g_mixed {};
std::array<uint64_t, 16> g_sixteen {};

float LUMEN_SYSV_ABI FloatValue(float value)
{
	g_float_input = BitsFromValue<uint32_t>(value);
	return ValueFromBits<float>(g_float_output);
}

double LUMEN_SYSV_ABI DoubleValue(double value)
{
	g_double_input = BitsFromValue<uint64_t>(value);
	return ValueFromBits<double>(g_double_output);
}

void LUMEN_SYSV_ABI VoidValue() { ++g_void_calls; }
uint64_t LUMEN_SYSV_ABI GpValue(uint64_t value) { return value; }
void* LUMEN_SYSV_ABI PointerValue(void* value) { return value; }
int32_t LUMEN_SYSV_ABI Signed32Value(int32_t value) { return value; }
int64_t LUMEN_SYSV_ABI Signed64Value(int64_t value) { return value; }
bool LUMEN_SYSV_ABI BooleanValue(bool value) { return value; }
BoolValue LUMEN_SYSV_ABI EnumBoolValue(BoolValue value) { return value; }
SignedValue LUMEN_SYSV_ABI EnumSignedValue(SignedValue value) { return value; }

void LUMEN_SYSV_ABI MixedValue(uint64_t integer, float single, double wide, const void* pointer,
                             int32_t signed_value, BoolValue flag)
{
	g_mixed = {integer, BitsFromValue<uint32_t>(single), BitsFromValue<uint64_t>(wide),
	           static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)),
	           static_cast<uint32_t>(signed_value), flag == BoolValue::Set ? UINT64_C(1) : UINT64_C(0)};
	++g_void_calls;
}

uint64_t LUMEN_SYSV_ABI NineValues(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t last)
{
	return last;
}

uint64_t LUMEN_SYSV_ABI OtherNineValues(uint64_t first, uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t)
{
	return first;
}

uint64_t LUMEN_SYSV_ABI TwelveValues(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t last)
{
	return last;
}

uint64_t LUMEN_SYSV_ABI SixteenValues(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                   uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11,
                                   uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15)
{
	g_sixteen = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15};
	return a15;
}

// Null function pointers select the signature traits without defining or
// instantiating an invoker for a signature the factory must reject.
[[maybe_unused]] void LUMEN_SYSV_ABI VariadicValue(uint64_t, ...);
[[maybe_unused]] void LUMEN_SYSV_ABI AggregateArgument(Aggregate);
[[maybe_unused]] Aggregate LUMEN_SYSV_ABI AggregateResult();
[[maybe_unused]] void LUMEN_SYSV_ABI SeventeenValues(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static_assert(!HostCallScalarDetail::SignatureSupported(static_cast<decltype(&VariadicValue)>(nullptr)));
static_assert(!HostCallScalarDetail::SignatureSupported(static_cast<decltype(&AggregateArgument)>(nullptr)));
static_assert(!HostCallScalarDetail::SignatureSupported(static_cast<decltype(&AggregateResult)>(nullptr)));
static_assert(!HostCallScalarDetail::SignatureSupported(static_cast<decltype(&SeventeenValues)>(nullptr)));
static_assert(HostCallScalarDetail::SignatureSupported(&SixteenValues));
static_assert(HostCallScalarDetail::SignatureSupported(&EnumBoolValue));
static_assert(sizeof(float) == 4 && sizeof(double) == 8);
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

constexpr auto k_float = MakeHostCallScalarMetadata<&FloatValue>();
constexpr auto k_double = MakeHostCallScalarMetadata<&DoubleValue>();
constexpr auto k_void = MakeHostCallScalarMetadata<&VoidValue>();
constexpr auto k_gp = MakeHostCallScalarMetadata<&GpValue>();
constexpr auto k_pointer = MakeHostCallScalarMetadata<&PointerValue>();
constexpr auto k_signed32 = MakeHostCallScalarMetadata<&Signed32Value>();
constexpr auto k_signed64 = MakeHostCallScalarMetadata<&Signed64Value>();
constexpr auto k_bool = MakeHostCallScalarMetadata<&BooleanValue>();
constexpr auto k_enum_bool = MakeHostCallScalarMetadata<&EnumBoolValue>();
constexpr auto k_enum_signed = MakeHostCallScalarMetadata<&EnumSignedValue>();
constexpr auto k_mixed = MakeHostCallScalarMetadata<&MixedValue>();
constexpr auto k_sixteen = MakeHostCallScalarMetadata<&SixteenValues>();
constexpr auto k_nine_scalar = MakeHostCallScalarMetadata<&NineValues>();
constexpr auto k_nine_gp = MakeHostCallGpMetadata<&NineValues>();
constexpr auto k_other_nine_gp = MakeHostCallGpMetadata<&OtherNineValues>();
constexpr auto k_twelve_gp = MakeHostCallGpMetadata<&TwelveValues>();

static_assert(k_void.arity == 0 && k_void.result == ScalarType::Void && k_void.invoker != nullptr);
static_assert(k_float.arity == 1 && k_float.arguments[0] == ScalarType::Float32 && k_float.result == ScalarType::Float32);
static_assert(k_double.arity == 1 && k_double.arguments[0] == ScalarType::Float64 && k_double.result == ScalarType::Float64);
static_assert(k_pointer.arguments[0] == ScalarType::Gp && k_pointer.result == ScalarType::Gp);
static_assert(k_signed32.result == ScalarType::Gp && k_enum_bool.result == ScalarType::Gp);
static_assert(k_mixed.arity == 6 && k_mixed.result == ScalarType::Void);
static_assert(k_mixed.arguments[0] == ScalarType::Gp && k_mixed.arguments[1] == ScalarType::Float32 &&
              k_mixed.arguments[2] == ScalarType::Float64 && k_mixed.arguments[3] == ScalarType::Gp &&
              k_mixed.arguments[4] == ScalarType::Gp && k_mixed.arguments[5] == ScalarType::Gp);
static_assert(k_sixteen.arity == 16 && k_sixteen.arguments[15] == ScalarType::Gp);
static_assert(k_nine_gp.arity == 9 && k_nine_gp.invoker != nullptr);
static_assert(k_twelve_gp.arity == 12 && k_twelve_gp.invoker != nullptr);

bool Equal(const HostCallScalarMetadata* actual, const HostCallScalarMetadata& expected)
{
	return actual != nullptr && actual->arity == expected.arity && actual->arguments == expected.arguments &&
	       actual->result == expected.result && actual->invoker == expected.invoker;
}

bool Equal(HostCallGpMetadata actual, HostCallGpMetadata expected)
{
	return actual.arity == expected.arity && actual.invoker == expected.invoker;
}

bool TestNativeConversions()
{
	constexpr std::array<std::array<uint32_t, 2>, 4> floats {{
	    {UINT32_C(0x80000000), UINT32_C(0x3fa00000)},
	    {UINT32_C(0x00000001), UINT32_C(0x807fffff)},
	    {UINT32_C(0x7fc12345), UINT32_C(0xff800000)},
	    {UINT32_C(0x7f800000), UINT32_C(0xffc34567)},
	}};
	for (const auto& pair: floats)
	{
		g_float_output = pair[1];
		const uint64_t input = UINT64_C(0xbacdef1200000000) | pair[0];
		if (!Check(k_float.invoker(&input) == pair[1] && g_float_input == pair[0],
		           "float input/output bits or upper-word clearing differ")) return false;
	}
	constexpr std::array<std::array<uint64_t, 2>, 4> doubles {{
	    {UINT64_C(0x8000000000000000), UINT64_C(0x3ff4000000000000)},
	    {UINT64_C(0x0000000000000001), UINT64_C(0x800fffffffffffff)},
	    {UINT64_C(0x7ff8123456789abc), UINT64_C(0xfff0000000000000)},
	    {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff8abcdef012345)},
	}};
	for (const auto& pair: doubles)
	{
		g_double_output = pair[1];
		if (!Check(k_double.invoker(&pair[0]) == pair[1] && g_double_input == pair[0],
		           "double input/output bits differ")) return false;
	}
	for (uint64_t word: {UINT64_C(0), UINT64_C(1), UINT64_C(0x8070605040302010), UINT64_MAX})
	{
		if (!Check(k_gp.invoker(&word) == word && k_signed64.invoker(&word) == word,
		           "64-bit GP or signed round trip lost bits") ||
		    !Check(k_bool.invoker(&word) == (word != 0 ? 1U : 0U) &&
		           k_enum_bool.invoker(&word) == (word != 0 ? 1U : 0U),
		           "bool or bool-underlying enum was not canonicalized")) return false;
	}
	const uint64_t signed_word = UINT64_C(0xa1b2c3d4ffffffef);
	const uint64_t enum_word = static_cast<uint16_t>(SignedValue::Negative);
	int local = 17;
	const uint64_t pointer_word = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&local));
	const uint64_t null_word = 0;
	if (!Check(k_signed32.invoker(&signed_word) == UINT64_C(0xffffffef), "signed 32-bit result was not width-limited") ||
	    !Check(k_enum_signed.invoker(&enum_word) == UINT64_C(0xfff5), "signed enum result was not width-limited") ||
	    !Check(k_pointer.invoker(&pointer_word) == pointer_word && k_pointer.invoker(&null_word) == 0,
	           "live or null pointer round trip differs")) return false;

	g_void_calls = 0;
	if (!Check(k_void.invoker(nullptr) == 0 && g_void_calls == 1, "zero-argument void invoker did not call once")) return false;
	const std::array<uint64_t, 6> mixed {UINT64_C(0xa34f892bcdef0123), UINT64_C(0x3fc00000),
	                                   UINT64_C(0xc002000000000000), pointer_word, UINT64_C(0xffffffd7), 1};
	if (!Check(k_mixed.invoker(mixed.data()) == 0 && g_void_calls == 2 && g_mixed == mixed,
	           "mixed native signature lost argument order or void completion")) return false;
	std::array<uint64_t, 16> sixteen {};
	for (size_t i = 0; i < sixteen.size(); ++i) sixteen[i] = UINT64_C(0x9384756000000000) + i;
	if (!Check(k_sixteen.invoker(sixteen.data()) == sixteen.back() && g_sixteen == sixteen,
	           "sixteen-argument boundary lost values") ||
	    !Check(k_nine_gp.invoker(sixteen.data()) == sixteen[8] && k_twelve_gp.invoker(sixteen.data()) == sixteen[11],
	           "extended GP factory boundary failed native invocation")) return false;
	for (uint32_t i = k_mixed.arity; i < HOST_CALL_SCALAR_MAX_ARITY; ++i)
	{
		if (!Check(k_mixed.arguments[i] == ScalarType::Void, "factory left nonempty unused argument metadata")) return false;
	}
	return true;
}

bool TestAliases()
{
	uint8_t identities[5] {};
	const uint64_t scalar_address = reinterpret_cast<uintptr_t>(&identities[0]);
	const uint64_t gp_address = reinterpret_cast<uintptr_t>(&identities[1]);
	SymbolDatabase symbols;
	if (!Check(symbols.HostCallScalarMetadataFor(scalar_address) == nullptr &&
	           Equal(symbols.HostCallGpMetadataFor(gp_address), {}), "empty database contains metadata") ||
	    !Check(symbols.SetHostCallScalarMetadata(scalar_address, k_nine_scalar) &&
	           symbols.SetHostCallScalarMetadata(scalar_address, k_nine_scalar), "identical scalar alias was refused") ||
	    !Check(!symbols.SetHostCallGpMetadata(scalar_address, k_nine_gp) &&
	           Equal(symbols.HostCallScalarMetadataFor(scalar_address), k_nine_scalar) &&
	           Equal(symbols.HostCallGpMetadataFor(scalar_address), {}), "GP insertion after scalar changed the binding") ||
	    !Check(symbols.SetHostCallGpMetadata(gp_address, k_nine_gp) &&
	           symbols.SetHostCallGpMetadata(gp_address, k_nine_gp), "identical GP alias was refused") ||
	    !Check(!symbols.SetHostCallScalarMetadata(gp_address, k_nine_scalar) &&
	           Equal(symbols.HostCallGpMetadataFor(gp_address), k_nine_gp) &&
	           symbols.HostCallScalarMetadataFor(gp_address) == nullptr, "scalar insertion after GP changed the binding")) return false;

	for (unsigned change = 0; change < 4; ++change)
	{
		auto conflict = k_nine_scalar;
		if (change == 0) conflict.arity = 8;
		if (change == 1) conflict.arguments[0] = ScalarType::Float64;
		if (change == 2) conflict.result = ScalarType::Void;
		if (change == 3) conflict.invoker = k_gp.invoker;
		if (!Check(!symbols.SetHostCallScalarMetadata(scalar_address, conflict) &&
		           Equal(symbols.HostCallScalarMetadataFor(scalar_address), k_nine_scalar),
		           "conflicting scalar alias replaced stored metadata")) return false;
	}
	auto gp_conflict = k_nine_gp;
	gp_conflict.arity = 10;
	if (!Check(!symbols.SetHostCallGpMetadata(gp_address, gp_conflict) &&
	           !symbols.SetHostCallGpMetadata(gp_address, k_other_nine_gp) &&
	           Equal(symbols.HostCallGpMetadataFor(gp_address), k_nine_gp), "conflicting GP alias replaced stored metadata")) return false;

	const uint64_t copied_address = reinterpret_cast<uintptr_t>(&identities[2]);
	auto caller_owned = k_float;
	if (!Check(symbols.SetHostCallScalarMetadata(copied_address, caller_owned), "caller-owned metadata was refused")) return false;
	caller_owned.arguments[0] = ScalarType::Gp;
	caller_owned.result = ScalarType::Void;
	caller_owned.invoker = nullptr;
	std::array<uint8_t, 128> borrowed_aliases {};
	for (auto& identity: borrowed_aliases)
	{
		const uint64_t address = reinterpret_cast<uintptr_t>(&identity);
		const auto* borrowed = symbols.HostCallScalarMetadataFor(copied_address);
		if (!Check(borrowed != nullptr && symbols.SetHostCallScalarMetadata(address, *borrowed) &&
		           Equal(symbols.HostCallScalarMetadataFor(address), k_float),
		           "registering a borrowed descriptor lost its value as the registry grew")) return false;
	}
	const uint64_t scalar_max = reinterpret_cast<uintptr_t>(&identities[3]);
	const uint64_t gp_max = reinterpret_cast<uintptr_t>(&identities[4]);
	return Check(symbols.SetHostCallScalarMetadata(scalar_max, k_sixteen) &&
	             Equal(symbols.HostCallScalarMetadataFor(scalar_max), k_sixteen) &&
	             symbols.SetHostCallGpMetadata(gp_max, k_twelve_gp) &&
	             Equal(symbols.HostCallGpMetadataFor(gp_max), k_twelve_gp), "maximum valid arity was refused") &&
	       Check(Equal(symbols.HostCallScalarMetadataFor(copied_address), k_float), "database retained caller-owned metadata storage") &&
	       Check(symbols.HostCallTargetCount() == 0, "metadata insertion published a host target");
}

bool TestInvalidMetadata()
{
	uint8_t identity = 0;
	const uint64_t address = reinterpret_cast<uintptr_t>(&identity);
	SymbolDatabase symbols;
	if (!Check(!symbols.SetHostCallScalarMetadata(0, k_float) && !symbols.SetHostCallGpMetadata(0, k_nine_gp) &&
	           symbols.HostCallScalarMetadataFor(0) == nullptr && Equal(symbols.HostCallGpMetadataFor(0), {}),
	           "zero address accepted metadata")) return false;
	std::array<HostCallScalarMetadata, 7> invalid {};
	invalid.fill(k_float);
	invalid[0].arity = HOST_CALL_SCALAR_MAX_ARITY + 1;
	invalid[1].invoker = nullptr;
	invalid[2].result = ScalarType::Unsupported;
	invalid[3].result = static_cast<ScalarType>(255);
	invalid[4].arguments[0] = ScalarType::Void;
	invalid[5].arguments[0] = ScalarType::Unsupported;
	invalid[6].arguments[0] = static_cast<ScalarType>(255);
	for (const auto& metadata: invalid)
	{
		if (!Check(!symbols.SetHostCallScalarMetadata(address, metadata) &&
		           symbols.HostCallScalarMetadataFor(address) == nullptr, "invalid scalar metadata created a binding")) return false;
	}
	const std::array<HostCallGpMetadata, 3> invalid_gp {{
	    {8, k_nine_gp.invoker}, {13, k_nine_gp.invoker}, {9, nullptr},
	}};
	for (const auto& metadata: invalid_gp)
	{
		if (!Check(!symbols.SetHostCallGpMetadata(address, metadata) && Equal(symbols.HostCallGpMetadataFor(address), {}),
		           "invalid GP metadata created a binding")) return false;
	}
	if (!Check(symbols.SetHostCallScalarMetadata(address, k_float), "valid scalar insertion after invalid attempts failed")) return false;
	for (const auto& metadata: invalid)
	{
		if (!Check(!symbols.SetHostCallScalarMetadata(address, metadata) &&
		           Equal(symbols.HostCallScalarMetadataFor(address), k_float), "invalid scalar metadata damaged an existing binding")) return false;
	}
	SymbolDatabase gp_symbols;
	if (!Check(gp_symbols.SetHostCallGpMetadata(address, k_nine_gp), "valid GP insertion failed")) return false;
	for (const auto& metadata: invalid_gp)
	{
		if (!Check(!gp_symbols.SetHostCallGpMetadata(address, metadata) &&
		           Equal(gp_symbols.HostCallGpMetadataFor(address), k_nine_gp), "invalid GP metadata damaged an existing binding")) return false;
	}
	return Check(symbols.HostCallTargetCount() == 0 && gp_symbols.HostCallTargetCount() == 0,
	             "validation published a host target");
}

bool TestSealAndLifetime()
{
	uint8_t identities[4] {};
	const uint64_t scalar_address = reinterpret_cast<uintptr_t>(&identities[0]);
	const uint64_t gp_address = reinterpret_cast<uintptr_t>(&identities[1]);
	const uint64_t new_scalar = reinterpret_cast<uintptr_t>(&identities[2]);
	const uint64_t new_gp = reinterpret_cast<uintptr_t>(&identities[3]);
	{
		SymbolDatabase symbols;
		if (!Check(!symbols.HostCallRegistrySealed() && symbols.SetHostCallScalarMetadata(scalar_address, k_void) &&
		           symbols.SetHostCallGpMetadata(gp_address, k_nine_gp), "pre-seal setup failed")) return false;
		symbols.SealHostCallRegistry();
		symbols.SealHostCallRegistry();
		if (!Check(symbols.HostCallRegistrySealed(), "authoritative registry did not seal") ||
		    !Check(!symbols.SetHostCallScalarMetadata(scalar_address, k_void) &&
		           !symbols.SetHostCallGpMetadata(gp_address, k_nine_gp), "seal allowed identical metadata re-registration") ||
		    !Check(!symbols.SetHostCallScalarMetadata(new_scalar, k_sixteen) &&
		           !symbols.SetHostCallGpMetadata(new_gp, k_nine_gp), "seal allowed a new metadata binding") ||
		    !Check(Equal(symbols.HostCallScalarMetadataFor(scalar_address), k_void) &&
		           Equal(symbols.HostCallGpMetadataFor(gp_address), k_nine_gp) &&
		           symbols.HostCallScalarMetadataFor(new_scalar) == nullptr &&
		           Equal(symbols.HostCallGpMetadataFor(new_gp), {}), "seal changed readable metadata") ||
		    !Check(symbols.HostCallTargetCount() == 0, "seal published a host target")) return false;
	}
	SymbolDatabase fresh;
	return Check(!fresh.HostCallRegistrySealed() && fresh.HostCallScalarMetadataFor(scalar_address) == nullptr &&
	             Equal(fresh.HostCallGpMetadataFor(gp_address), {}) && fresh.HostCallTargetCount() == 0,
	             "a fresh database inherited metadata or seal state");
}

bool TestDispatchSnapshot()
{
	const uint64_t scalar_address = reinterpret_cast<uint64_t>(&FloatValue);
	const uint64_t gp_address = reinterpret_cast<uint64_t>(&NineValues);
	const uint64_t variadic_address = reinterpret_cast<uint64_t>(&GpValue);
	const uint64_t plain_address = reinterpret_cast<uint64_t>(&VoidValue);
	uint8_t identities[2] {};
	const uint64_t metadata_only = reinterpret_cast<uint64_t>(&identities[0]);
	const uint64_t missing = reinterpret_cast<uint64_t>(&identities[1]);
	SymbolDatabase symbols;
	if (!Check(symbols.ResolveHostCall(missing).scalar == nullptr &&
	           symbols.ResolveHostCall(missing).classification == HostCallRegistryClassification::Unsealed,
	           "mutable dispatch miss was classified or retained metadata") ||
	    !Check(symbols.SetHostCallScalarMetadata(metadata_only, k_void) &&
	           symbols.SetHostCallEffects(metadata_only, Lumen::GuestCpu::HostCallEffect::None) &&
	           symbols.SetHostCallScalarMetadata(plain_address, k_void) &&
	           symbols.SetHostCallEffects(plain_address, Lumen::GuestCpu::HostCallEffect::None) &&
	           symbols.SetHostCallScalarMetadata(scalar_address, k_float) &&
	           symbols.SetHostCallGpMetadata(gp_address, k_nine_gp) &&
	           symbols.SetHostCallVariadicKind(variadic_address, HostCallVariadicKind::Printf),
	           "dispatch metadata setup failed")) return false;
	for (uint64_t address: {scalar_address, gp_address, variadic_address, plain_address})
		if (!Check(symbols.RegisterHostCallTarget(address), "dispatch host registration failed")) return false;
	const auto* original_scalar = symbols.HostCallScalarMetadataFor(scalar_address);
	const auto matches_original = [&](uint64_t address) {
		const auto resolved = symbols.ResolveHostCall(address);
		return resolved.classification == symbols.ClassifyHostCallTarget(address) &&
		       resolved.scalar == symbols.HostCallScalarMetadataFor(address) &&
		       Equal(resolved.gp, symbols.HostCallGpMetadataFor(address)) &&
		       resolved.variadic == symbols.HostCallVariadicKindFor(address);
	};
	for (uint64_t address: {uint64_t{0}, scalar_address, gp_address, variadic_address, plain_address, metadata_only, missing})
		if (!Check(matches_original(address), "mutable dispatch changed registry/ABI behavior")) return false;
	symbols.SealHostCallRegistry();
	for (uint64_t address: {uint64_t{0}, scalar_address, gp_address, variadic_address, plain_address, metadata_only, missing})
		if (!Check(matches_original(address), "sealed dispatch changed registry/ABI behavior")) return false;
	if (!Check(symbols.ResolveHostCall(metadata_only).classification == HostCallRegistryClassification::Guest &&
	           symbols.ResolveHostCall(metadata_only).scalar != nullptr &&
	           symbols.ResolveHostCall(plain_address).effects == Lumen::GuestCpu::HostCallEffect::None &&
	           symbols.ResolveHostCall(missing).effects == Lumen::GuestCpu::HostCallEffect::Unknown &&
	           symbols.ResolveHostCall(scalar_address).scalar == original_scalar,
	           "snapshot published metadata-only host or changed scalar ownership")) return false;
	std::atomic<bool> concurrent_ok {true};
	std::array<std::thread, 4> readers;
	for (auto& reader: readers) reader = std::thread([&] {
		for (unsigned round = 0; round < 2000; ++round)
			if (!matches_original(scalar_address) || !matches_original(gp_address) ||
			    !matches_original(variadic_address) || !matches_original(missing))
				concurrent_ok.store(false, std::memory_order_relaxed);
	});
	// Idempotent sealing must not rebuild the storage under concurrent readers.
	symbols.SealHostCallRegistry();
	for (auto& reader: readers) reader.join();
	SymbolDatabase fresh;
	const auto fresh_result = fresh.ResolveHostCall(scalar_address);
	return Check(concurrent_ok.load(std::memory_order_relaxed), "concurrent snapshot reads changed metadata") &&
	       Check(fresh_result.classification == HostCallRegistryClassification::Unsealed && fresh_result.scalar == nullptr &&
	             Equal(fresh_result.gp, {}) && fresh_result.variadic == HostCallVariadicKind::None,
	             "dispatch snapshot escaped its database lifetime");
}

bool TestEffects()
{
	using Effect = Lumen::GuestCpu::HostCallEffect;
	uint8_t identities[3] {};
	const auto pure = reinterpret_cast<uint64_t>(&identities[0]);
	const auto legacy = reinterpret_cast<uint64_t>(&identities[1]);
	const auto float_or_spill = reinterpret_cast<uint64_t>(&identities[2]);
	SymbolDatabase symbols;
	if (!Check(symbols.ResolveHostCall(pure).effects == Effect::Unknown &&
	           !symbols.SetHostCallEffects(0, Effect::None) &&
	           !symbols.SetHostCallEffects(legacy, Effect::None), "unknown/legacy effects admitted light bridge") ||
	    !Check(symbols.SetHostCallScalarMetadata(pure, k_gp) &&
	           symbols.SetHostCallEffects(pure, Effect::None) &&
	           symbols.SetHostCallEffects(pure, Effect::None) &&
	           !symbols.SetHostCallEffects(pure, Effect::Blocking) &&
	           !symbols.SetHostCallEffects(pure, static_cast<Effect>(1U << 20)),
	           "effect alias conflict or invalid mask was accepted") ||
	    !Check(symbols.SetHostCallScalarMetadata(float_or_spill, k_sixteen) &&
	           !symbols.SetHostCallEffects(float_or_spill, Effect::None) &&
	           symbols.SetHostCallEffects(float_or_spill, Effect::GuestMemory | Effect::Blocking),
	           "spilled pure signature admitted light bridge") ||
	    !Check(symbols.SetHostCallScalarMetadata(legacy, k_float) &&
	           !symbols.SetHostCallEffects(legacy, Effect::None), "FP signature admitted GP light bridge")) return false;
	symbols.SealHostCallRegistry();
	SymbolDatabase fresh;
	return Check(symbols.ResolveHostCall(pure).classification == HostCallRegistryClassification::Guest &&
	             symbols.ResolveHostCall(pure).effects == Effect::None &&
	             symbols.ResolveHostCall(float_or_spill).effects == (Effect::GuestMemory | Effect::Blocking) &&
	             !symbols.SetHostCallEffects(pure, Effect::None) && symbols.HostCallTargetCount() == 0 &&
	             fresh.ResolveHostCall(pure).effects == Effect::Unknown,
	             "effects changed authority, survived database lifetime, or mutated after sealing");
}

bool TestFastIndexCollisions()
{
	using Index = Lumen::GuestCpu::HostCallFastIndex;
	using Lumen::GuestCpu::HostCallFastIndexRequested;
	if (!Check(!HostCallFastIndexRequested(nullptr) && !HostCallFastIndexRequested("") &&
	           !HostCallFastIndexRequested("0") && !HostCallFastIndexRequested("true") &&
	           !HostCallFastIndexRequested("01") && HostCallFastIndexRequested("1"),
	           "fast-index selector did not require exact one")) return false;
	std::vector<uint64_t> addresses;
	uint64_t candidate = 1;
	while (addresses.size() != 16)
	{
		if ((Index::Hash(candidate) & 63) == 0) addresses.push_back(candidate);
		++candidate;
	}
	while ((Index::Hash(candidate) & 63) != 0) ++candidate;
	const uint64_t colliding_miss = candidate;
	// Metadata-only addresses must stay guest-classified even when their full
	// keys collide. This test never invokes the synthetic addresses.
	SymbolDatabase symbols;
	for (size_t i = 0; i != addresses.size(); ++i)
		if (!Check(symbols.SetHostCallScalarMetadata(addresses[i], i % 2 == 0 ? k_float : k_void),
		           "colliding metadata insertion failed")) return false;
	symbols.SealHostCallRegistry();
	for (size_t i = 0; i != addresses.size(); ++i)
	{
		const auto result = symbols.ResolveHostCall(addresses[i]);
		if (!Check(result.classification == HostCallRegistryClassification::Guest &&
		           Equal(result.scalar, i % 2 == 0 ? k_float : k_void),
		           "colliding dispatch changed exact metadata or granted host authority")) return false;
	}
	for (uint64_t address: {UINT64_C(0), colliding_miss, UINT64_MAX})
		if (!Check(symbols.ResolveHostCall(address).scalar == nullptr &&
		           symbols.ResolveHostCall(address).classification == HostCallRegistryClassification::Guest,
		           "colliding or sentinel miss matched metadata")) return false;
	if (!Check(!symbols.SetHostCallScalarMetadata(colliding_miss, k_void),
	           "indexed database admitted insertion after seal")) return false;

	// The generic index supports the complete key domain; runtime authority
	// separately rejects zero/UINT64_MAX. Also verify first-ordinal aliases and
	// that modifying the construction input cannot change an owned snapshot.
	addresses.push_back(0);
	addresses.push_back(UINT64_MAX);
	addresses.push_back(addresses.front());
	const Index index(addresses);
	const auto original = addresses;
	addresses.assign(2048, colliding_miss);
	for (size_t i = 0; i != original.size() - 1; ++i)
		if (!Check(index.Find(original[i]) == i, "index collision, sentinel or ownership mismatch")) return false;
	const Index empty;
	return Check(index.Find(colliding_miss) == Index::NotFound &&
	             empty.Find(0) == Index::NotFound && empty.Find(original.front()) == Index::NotFound,
	             "missing or empty index lookup admitted an address");
}

} // namespace

int main(int argc, char** argv)
{
	const char* fast_index = nullptr;
	if (argc == 2 && std::strcmp(argv[1], "--fast-index") == 0) fast_index = "1";
	else if (argc == 2 && std::strcmp(argv[1], "--legacy-index") == 0) fast_index = "0";
	else if (argc != 1) return 2;
#if defined(_WIN32)
	const int environment_result = _putenv_s("LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY", "1");
	if (fast_index != nullptr && _putenv_s("LUMEN_FEX_HOST_CALL_FAST_INDEX", fast_index) != 0) return 1;
#else
	const int environment_result = setenv("LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY", "1", 1);
	if (fast_index != nullptr && setenv("LUMEN_FEX_HOST_CALL_FAST_INDEX", fast_index, 1) != 0) return 1;
#endif
	if (!Check(environment_result == 0, "could not select authoritative metadata sealing") ||
	    !Check(fast_index == nullptr || Lumen::GuestCpu::HostCallFastIndexEnabled() == (fast_index[0] == '1'),
	           "process selector did not match requested index path") ||
	    !TestNativeConversions() || !TestAliases() || !TestInvalidMetadata() || !TestSealAndLifetime() ||
	    !TestDispatchSnapshot() || !TestEffects() || !TestFastIndexCollisions()) return 1;
	std::printf("HostCallScalarMetadataTest: PASS fast_index=%u\n",
	            Lumen::GuestCpu::HostCallFastIndexEnabled() ? 1u : 0u);
	return 0;
}
