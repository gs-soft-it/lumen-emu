#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Log.h"

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include "Lumen/Core/VirtualMemory.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {

namespace Save = Lumen::Libs::Dialog::SaveDataDialog;
using Result = Save::SaveDataDialogResult;
using NoArguments = uint32_t (LUMEN_SYSV_ABI *)();
using Open = uint32_t (LUMEN_SYSV_ABI *)(const void*);
using GetResult = uint32_t (LUMEN_SYSV_ABI *)(Result*);
using Progress = uint32_t (LUMEN_SYSV_ABI *)(uint32_t, uint32_t);

// These ABI checks use only SAVE_DATA_DIALOG_INTERFACE.md's approved facts
// and Lumen's selected 64-bit layout. Runtime expectations below preserve the
// existing local immediate-completion policy; they do not assert console UI,
// validation, lifecycle, pointer ownership or reserved-byte semantics.
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogInitialize), NoArguments>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogOpen), Open>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogGetStatus), NoArguments>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogGetResult), GetResult>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogUpdateStatus), NoArguments>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogTerminate), NoArguments>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogProgressBarSetValue), Progress>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogProgressBarInc), Progress>);
static_assert(std::is_same_v<decltype(&Save::SaveDataDialogClose), NoArguments>);
static_assert(std::is_standard_layout_v<Result> && std::is_trivially_copyable_v<Result>);
static_assert(sizeof(Result) == 72 && alignof(Result) == 8);
static_assert(offsetof(Result, mode) == 0 && std::is_same_v<decltype(Result::mode), uint32_t>);
static_assert(offsetof(Result, result) == 4 && std::is_same_v<decltype(Result::result), uint32_t>);
static_assert(offsetof(Result, buttonId) == 8 && std::is_same_v<decltype(Result::buttonId), uint32_t>);
static_assert(offsetof(Result, dirName) == 16 && std::is_same_v<decltype(Result::dirName), uint64_t>);
static_assert(offsetof(Result, param) == 24 && std::is_same_v<decltype(Result::param), uint64_t>);
static_assert(offsetof(Result, userData) == 32 && std::is_same_v<decltype(Result::userData), uint64_t>);
static_assert(offsetof(Result, reserved) == 40 && std::is_same_v<decltype(Result::reserved), uint8_t[32]>);

constexpr uint8_t SENTINEL = 0xa7;
constexpr size_t LOCAL_WRITE_BYTES = 48;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "SaveDataDialogTest: %s\n", message);
	return condition;
}

bool BytesAre(const void* data, size_t size, uint8_t value)
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t index = 0; index < size; ++index)
		if (bytes[index] != value) return false;
	return true;
}

bool HasStatus(uint32_t expected)
{
	return Check(Save::SaveDataDialogGetStatus() == expected &&
	             Save::SaveDataDialogUpdateStatus() == expected,
	             "local status or polling result changed");
}

bool OpenFinished()
{
	return Check(Save::SaveDataDialogInitialize() == 0u, "local initialization failed") && HasStatus(1u) &&
	       Check(Save::SaveDataDialogOpen(nullptr) == 0u, "local open failed") && HasStatus(3u);
}

bool ReadWithCanaries()
{
	struct GuardedResult
	{
		std::array<uint8_t, 16> before;
		Result result;
		std::array<uint8_t, 16> after;
	};
	static_assert(offsetof(GuardedResult, result) == 16 && offsetof(GuardedResult, after) == 88);
	GuardedResult output {};
	std::memset(&output, SENTINEL, sizeof(output));
	return Check(Save::SaveDataDialogGetResult(&output.result) == 0u, "native result buffer was rejected") &&
	       Check(BytesAre(&output.result, LOCAL_WRITE_BYTES, 0), "local result did not initialize its historical 48-byte footprint") &&
	       Check(BytesAre(reinterpret_cast<const uint8_t*>(&output.result) + LOCAL_WRITE_BYTES,
	                      sizeof(Result) - LOCAL_WRITE_BYTES, SENTINEL),
	             "local result changed bytes 48 through 71 beyond its historical footprint") &&
	       Check(BytesAre(output.before.data(), output.before.size(), SENTINEL) &&
	             BytesAre(output.after.data(), output.after.size(), SENTINEL),
	             "result write crossed a record boundary") && HasStatus(0u);
}

bool TestLocalLifecycle()
{
	if (!HasStatus(0u) || !OpenFinished() || !ReadWithCanaries()) return false;
	if (!OpenFinished() || !Check(Save::SaveDataDialogGetResult(nullptr) == 0u,
	                             "local null-result behavior changed") || !HasStatus(0u)) return false;
	if (!OpenFinished() || !Check(Save::SaveDataDialogClose() == 0u, "local close failed") || !HasStatus(0u)) return false;
	return OpenFinished() && Check(Save::SaveDataDialogTerminate() == 0u, "local termination failed") && HasStatus(0u);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
namespace Vm = Lumen::Core::VirtualMemory;
constexpr size_t VM_UNIT = 65536;
constexpr uint32_t PARAM_INVALID = 0x80b8000au;

class Mapping
{
public:
	Mapping(): m_address(Vm::Alloc(0, 2 * VM_UNIT, Vm::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Vm::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(m_address + offset); }
	bool Protect(size_t offset, size_t size, Vm::Mode mode)
	{
		return Vm::Protect(m_address + offset, size, mode);
	}

private:
	uint64_t m_address;
};

bool TestProtectedOutput(bool cross_guard, size_t accessible_prefix = 0)
{
	Mapping mapping;
	if (!Check(mapping.Valid(), "protected output allocation failed")) return false;
	std::memset(mapping.Data(), SENTINEL, 2 * VM_UNIT);
	// The public record describes 72 bytes, but Lumen preserves its historical
	// 48-byte write. Exactly 48 writable bytes must suffice; 47 must reject
	// atomically. Neither case requires touching the inaccessible suffix.
	const size_t output_offset = cross_guard ? VM_UNIT - accessible_prefix : 32;
	const size_t protected_offset = cross_guard ? VM_UNIT : 0;
	const auto mode = cross_guard ? Vm::Mode::NoAccess : Vm::Mode::Read;
	if (!Check(mapping.Protect(protected_offset, VM_UNIT, mode), "output protection failed") ||
	    !OpenFinished()) return false;
	const uint32_t returned = Save::SaveDataDialogGetResult(reinterpret_cast<Result*>(mapping.Data(output_offset)));
	const bool expected_success = cross_guard && accessible_prefix == LOCAL_WRITE_BYTES;
	const bool state_correct = HasStatus(expected_success ? 0u : 3u);
	if (!Check(mapping.Protect(0, 2 * VM_UNIT, Vm::Mode::ReadWrite), "output protection restore failed")) return false;
	if (!Check(returned == (expected_success ? 0u : PARAM_INVALID),
	           expected_success ? "48 writable bytes before the guard were rejected" : "protected result was not rejected") ||
	    !state_correct) return false;
	for (size_t index = 0; index < 2 * VM_UNIT; ++index)
	{
		const uint8_t expected = expected_success && index >= output_offset && index < output_offset + LOCAL_WRITE_BYTES
		                             ? uint8_t {0} : SENTINEL;
		if (!Check(mapping.Data()[index] == expected, "protected result changed bytes outside the permitted footprint")) return false;
	}
	return expected_success || ReadWithCanaries();
}
#endif

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	Vm::Init();
#endif
	if (!TestLocalLifecycle()) return 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (!TestProtectedOutput(false) || !TestProtectedOutput(true, 48) || !TestProtectedOutput(true, 47)) return 1;
#endif
	std::puts("SaveDataDialogTest: PASS");
	return 0;
}
