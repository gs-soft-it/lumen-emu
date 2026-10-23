#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

namespace Pad = Lumen::Libs::Controller;
namespace Posix = Lumen::Libs::Posix;
namespace Vm = Lumen::Core::VirtualMemory;

// Only the invalid-handle label/number is an approved external interface fact.
// Precedence and non-mutation below test PAD_HANDLE_INTERFACE.md's local
// policy; the measured default-record refusals (device class, read state)
// follow the console campaigns of 28-29 September 2026.
constexpr uint32_t INVALID_HANDLE_BITS = 0x80920003u;
constexpr int ERRNO_SENTINEL = 17639;
constexpr std::array<int, 6> INVALID_HANDLES = {
    -1, 0, 3, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), Pad::VIRTUAL_CONTROLLER_ID};
constexpr std::array<int, 5> INVALID_COUNTS = {
    -1, 0, 65, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
static_assert(sizeof(int) == sizeof(int32_t));

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "PadHandleTest: %s\n", message);
	return condition;
}

// The private Pad records remain opaque. This is oversized aligned storage,
// not a replacement declaration of their layout or a console format claim.
struct Storage
{
	alignas(std::max_align_t) std::array<uint8_t, 16384> bytes {};

	Storage() { bytes.fill(0xa7); }
	void* Data() { return bytes.data() + 64; }
	uint32_t Buttons() const
	{
		// The original local PadData begins with its uint32_t buttons member.
		uint32_t buttons = 0;
		std::memcpy(&buttons, bytes.data() + 64, sizeof(buttons));
		return buttons;
	}
};

// Default device-less record measured on the console (oracle LMNQ92814):
// centered sticks (offsets 4..7), identity orientation w (24..27) and gravity
// on the Y axis (32..35), every other byte zero. The window matches
// Emulator/Controller.h's PAD_DATA_RECORD_SIZE.
std::array<uint8_t, Pad::PAD_DATA_RECORD_SIZE> DeviceLessRecord()
{
	std::array<uint8_t, Pad::PAD_DATA_RECORD_SIZE> record {};
	record[4] = record[5] = record[6] = record[7] = 0x80;
	const float one = 1.0f;
	std::memcpy(record.data() + 24, &one, sizeof(one));
	std::memcpy(record.data() + 32, &one, sizeof(one));
	return record;
}

class NoAccessMapping
{
public:
	NoAccessMapping(): m_address(Vm::Alloc(0, 65536, Vm::Mode::NoAccess)) {}
	~NoAccessMapping() { if (m_address != 0) Vm::Free(m_address); }
	NoAccessMapping(const NoAccessMapping&) = delete;
	NoAccessMapping& operator=(const NoAccessMapping&) = delete;
	bool Valid() const { return m_address != 0; }
	void* Data() const { return reinterpret_cast<void*>(m_address); }

private:
	uint64_t m_address;
};

enum class Operation { Information, DeviceClass, ReadState, Read, Motion, Vibration, ResetLight, Light };
constexpr std::array<Operation, 8> OPERATIONS = {
    Operation::Information, Operation::DeviceClass, Operation::ReadState, Operation::Read, Operation::Motion,
    Operation::Vibration, Operation::ResetLight, Operation::Light};

const char* Name(Operation operation)
{
	switch (operation)
	{
		case Operation::Information: return "GetControllerInformation";
		case Operation::DeviceClass: return "DeviceClassGetExtendedInformation";
		case Operation::ReadState: return "ReadState";
		case Operation::Read: return "Read";
		case Operation::Motion: return "SetMotionSensorState";
		case Operation::Vibration: return "SetVibration";
		case Operation::ResetLight: return "ResetLightBar";
		case Operation::Light: return "SetLightBar";
	}
	std::abort();
}

int Invoke(Operation operation, int handle, void* parameter, int count = 1)
{
	switch (operation)
	{
		case Operation::Information:
			return Pad::PadGetControllerInformation(handle, reinterpret_cast<Pad::PadControllerInformation*>(parameter));
		case Operation::DeviceClass:
			return Pad::PadDeviceClassGetExtendedInformation(
			    handle, reinterpret_cast<Pad::PadDeviceClassExtendedInformation*>(parameter));
		case Operation::ReadState: return Pad::PadReadState(handle, reinterpret_cast<Pad::PadData*>(parameter));
		case Operation::Read: return Pad::PadRead(handle, reinterpret_cast<Pad::PadData*>(parameter), count);
		case Operation::Motion: return Pad::PadSetMotionSensorState(handle, count != 0);
		case Operation::Vibration: return Pad::PadSetVibration(handle, reinterpret_cast<Pad::PadVibrationParam*>(parameter));
		case Operation::ResetLight: return Pad::PadResetLightBar(handle);
		case Operation::Light: return Pad::PadSetLightBar(handle, reinterpret_cast<Pad::PadLightBarParam*>(parameter));
	}
	std::abort();
}

bool Rejected(Operation operation, int handle, void* parameter, int count, const char* phase)
{
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const int result = Invoke(operation, handle, parameter, count);
	if (result >= 0 || static_cast<uint32_t>(result) != INVALID_HANDLE_BITS || *Posix::GetErrorAddr() != ERRNO_SENTINEL)
	{
		std::fprintf(stderr, "PadHandleTest: %s %s handle=%d count=%d returned 0x%08x, errno=%d\n",
		             phase, Name(operation), handle, count, static_cast<unsigned>(result), *Posix::GetErrorAddr());
		return false;
	}
	return true;
}

bool Accepted(Operation operation, void* parameter, int expected = 0, int count = 1)
{
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	const int result = Invoke(operation, 1, parameter, count);
	if (result != expected || *Posix::GetErrorAddr() != ERRNO_SENTINEL)
	{
		std::fprintf(stderr, "PadHandleTest: valid %s returned %d instead of %d, errno=%d\n",
		             Name(operation), result, expected, *Posix::GetErrorAddr());
		return false;
	}
	return true;
}

bool TestBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libPad_1", &symbols), "Pad registration failed")) return false;
	struct Binding { const char* name; uint64_t address; };
	const std::array<Binding, 10> bindings = {{
	    {"hv1luiJrqQM", reinterpret_cast<uint64_t>(&Pad::PadInit)},
	    {"xk0AcarP3V4", reinterpret_cast<uint64_t>(&Pad::PadOpen)},
	    {"clVvL4ZDntw", reinterpret_cast<uint64_t>(&Pad::PadSetMotionSensorState)},
	    {"gjP9-KQzoUk", reinterpret_cast<uint64_t>(&Pad::PadGetControllerInformation)},
	    {"AcslpN1jHR8", reinterpret_cast<uint64_t>(&Pad::PadDeviceClassGetExtendedInformation)},
	    {"YndgXqQVV7c", reinterpret_cast<uint64_t>(&Pad::PadReadState)},
	    {"q1cHNfGycLI", reinterpret_cast<uint64_t>(&Pad::PadRead)},
	    {"yFVnOdGxvZY", reinterpret_cast<uint64_t>(&Pad::PadSetVibration)},
	    {"DscD1i9HX1w", reinterpret_cast<uint64_t>(&Pad::PadResetLightBar)},
	    {"RR4novUEENY", reinterpret_cast<uint64_t>(&Pad::PadSetLightBar)}}};
	for (const auto& binding : bindings)
	{
		Lumen::Loader::SymbolResolve exact {};
		exact.name = binding.name;
		exact.library = "Pad";
		exact.library_version = 1;
		exact.module = "Pad";
		exact.module_version_major = 1;
		exact.module_version_minor = 1;
		exact.type = Lumen::Loader::SymbolType::Func;
		const auto* symbol = symbols.Find(exact);
		if (!Check(symbol != nullptr && symbol->vaddr == binding.address, "existing Pad binding changed")) return false;
		for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
		{
			auto wrong = exact;
			switch (mismatch)
			{
				case 0: wrong.library = "unrelated_pad_library"; break;
				case 1: wrong.module = "unrelated_pad_module"; break;
				case 2: wrong.library_version = 2; break;
				case 3: wrong.module_version_major = 2; break;
				case 4: wrong.module_version_minor = 0; break;
				case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
			}
			if (!Check(symbols.Find(wrong) == nullptr, "Pad binding accepted an unsupported identity")) return false;
		}
	}
	return true;
}

bool TestInvalidMatrix(void* inaccessible, const char* phase)
{
	Storage storage;
	const auto before = storage.bytes;
	const std::array<void*, 5> pointers = {
	    storage.Data(), nullptr, inaccessible, reinterpret_cast<void*>(uintptr_t {1}),
	    reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max())};
	for (int handle : INVALID_HANDLES)
	{
		for (void* pointer : pointers)
		{
			for (Operation operation : OPERATIONS)
			{
				// The measured device-class profile stores its default 20-byte
				// record even for a rejected handle, so only its no-write
				// null pointer is exercised here; the record write itself is
				// checked with writable storage in TestDeviceClassInformation.
				if (operation == Operation::DeviceClass)
				{
					if (!Rejected(operation, handle, nullptr, 1, phase)) return false;
					continue;
				}
				// The measured read-state refusal stores the default device-less
				// record for a refused handle, so only the no-write null pointer
				// is exercised here; the record write itself is checked with
				// writable storage in TestSpecialPortReads.
				if (operation == Operation::ReadState)
				{
					if (!Rejected(operation, handle, nullptr, 1, phase)) return false;
					continue;
				}
				if (!Rejected(operation, handle, pointer, 1, phase) ||
				    !Check(storage.bytes == before, "invalid handle changed parameter/output bytes")) return false;
			}
			if (!Rejected(Operation::Motion, handle, pointer, 0, phase) ||
			    !Rejected(Operation::Read, handle, pointer, 64, phase)) return false;
			for (int count : INVALID_COUNTS)
			{
				if (!Rejected(Operation::Read, handle, pointer, count, phase) ||
				    !Check(storage.bytes == before, "invalid handle/count changed output bytes")) return false;
			}
		}
	}
	return Check(storage.bytes == before, "invalid handle changed canaries");
}

bool TestValidControls()
{
	Storage parameter;
	const auto before = parameter.bytes;
	if (!Accepted(Operation::Motion, nullptr, 0, 0) || !Accepted(Operation::Motion, nullptr, 0, 1) ||
	    !Accepted(Operation::Vibration, parameter.Data()) || !Accepted(Operation::ResetLight, nullptr) ||
	    !Accepted(Operation::Light, parameter.Data()) ||
	    !Check(parameter.bytes == before, "valid controls changed input parameters")) return false;
	Pad::ControllerResetForNextSession();
	Storage data;
	return Accepted(Operation::ReadState, data.Data()) && Check(data.Buttons() == 0, "neutral valid ReadState changed") &&
	       Accepted(Operation::Read, data.Data(), 1, 1) && Check(data.Buttons() == 0, "neutral valid Read changed") &&
	       Accepted(Operation::Read, data.Data(), 1, 64) && Check(data.Buttons() == 0, "valid Read maximum count changed");
}

bool TestPadOpenDomain()
{
	// Port-type domain measured on the console (oracle LMNQ92801): the
	// standard (0) and special (2) types open, every other value returns the
	// measured invalid-argument error with precedence over the user check.
	constexpr uint32_t INVALID_ARGUMENT_BITS = 0x80920001u;
	const std::array<int, 5> invalid_types = {
	    1, 3, -1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
	for (int type : invalid_types)
	{
		for (int user : {1, 0, -1})
		{
			*Posix::GetErrorAddr() = ERRNO_SENTINEL;
			const int result = Pad::PadOpen(user, type, 0, nullptr);
			if (!Check(result == Pad::PAD_ERROR_INVALID_ARGUMENT && static_cast<uint32_t>(result) == INVALID_ARGUMENT_BITS &&
			               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
			           "invalid port type did not return the measured invalid-argument")) return false;
		}
	}
	for (int call = 0; call < 2; ++call)
	{
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		if (!Check(Pad::PadOpen(1, Pad::PAD_PORT_TYPE_STANDARD, 0, nullptr) == Pad::PAD_STANDARD_PORT_HANDLE &&
		               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
		           "standard port open changed")) return false;
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		if (!Check(Pad::PadOpen(1, Pad::PAD_PORT_TYPE_SPECIAL, 0, nullptr) == Pad::PAD_SPECIAL_PORT_HANDLE &&
		               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
		           "special port open changed")) return false;
	}
	return true;
}

bool TestSpecialPortInformation()
{
	// The device-less special port publishes the measured console values
	// (connection_type=0, connected_count=0, connected=false, device_class=0)
	// and keeps the local modeled geometry; the write stays bounded to the
	// modeled record and the query never consults live controller state.
	// Record offsets of the private local declaration: connection_type 10,
	// connected_count 11, connected 12, device_class 16.
	Storage special;
	const auto before = special.bytes;
	for (int call = 0; call < 2; ++call)
	{
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadGetControllerInformation(Pad::PAD_SPECIAL_PORT_HANDLE,
		                                                    reinterpret_cast<Pad::PadControllerInformation*>(special.Data()));
		int32_t device_class = -1;
		std::memcpy(&device_class, static_cast<const uint8_t*>(special.Data()) + 16, sizeof(device_class));
		const auto* bytes = static_cast<const uint8_t*>(special.Data());
		if (!Check(result == 0 && *Posix::GetErrorAddr() == ERRNO_SENTINEL && bytes[10] == 0 && bytes[11] == 0 &&
		               bytes[12] == 0 && device_class == Pad::PAD_MODELED_DEVICE_CLASS,
		           "device-less special port information changed")) return false;
	}
	for (std::size_t i = 0; i < special.bytes.size(); ++i)
	{
		const bool inside = i >= 64 && i < 64 + 32;
		if (!inside && special.bytes[i] != before[i])
		{
			return Check(false, "special port information wrote outside the modeled record");
		}
	}
	Storage standard;
	if (!Check(Pad::PadGetControllerInformation(Pad::PAD_STANDARD_PORT_HANDLE,
	                                           reinterpret_cast<Pad::PadControllerInformation*>(standard.Data())) == 0 &&
	               std::memcmp(standard.Data(), special.Data(), 10) == 0,
	           "modeled geometry differs between the two ports")) return false;
	return true;
}

bool TestDeviceClassInformation()
{
	// Measured console profile (oracle LMNQ92802): every call stores the
	// 20-byte record window (32-bit device class plus a zeroed 16-byte area).
	// The standard port reports class 0 and the device-less special port
	// reports class -1 (Invalid); a rejected handle still receives the default
	// record (class 0) with the shared invalid-handle error, while a null
	// output keeps the family no-write rule. Bytes outside the window keep
	// their canaries in every case.
	Storage storage;
	auto expected = storage.bytes;
	std::fill(expected.begin() + 64, expected.begin() + 84, uint8_t {0});
	if (!Accepted(Operation::DeviceClass, storage.Data(), 0) ||
	    !Check(storage.bytes == expected, "valid device-class information did not publish exactly the modeled window")) return false;
	Storage repeated;
	auto repeated_expected = repeated.bytes;
	std::fill(repeated_expected.begin() + 64, repeated_expected.begin() + 84, uint8_t {0});
	if (!Accepted(Operation::DeviceClass, repeated.Data(), 0) ||
	    !Check(repeated.bytes == repeated_expected, "repeated device-class information is not stable")) return false;

	Storage special;
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	if (!Check(Pad::PadDeviceClassGetExtendedInformation(
	               Pad::PAD_SPECIAL_PORT_HANDLE,
	               reinterpret_cast<Pad::PadDeviceClassExtendedInformation*>(special.Data())) == 0 &&
	               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
	           "device-less special port device-class information changed its result")) return false;
	auto special_expected = special.bytes;
	std::fill(special_expected.begin() + 64, special_expected.begin() + 84, uint8_t {0});
	const int32_t invalid_class = Pad::PAD_DEVICE_CLASS_INVALID;
	std::memcpy(special_expected.data() + 64, &invalid_class, sizeof(invalid_class));
	if (!Check(special.bytes == special_expected,
	           "special port device-class information did not publish the measured invalid class")) return false;

	for (int handle : INVALID_HANDLES)
	{
		Storage refused;
		auto refused_expected = refused.bytes;
		std::fill(refused_expected.begin() + 64, refused_expected.begin() + 84, uint8_t {0});
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadDeviceClassGetExtendedInformation(
		    handle, reinterpret_cast<Pad::PadDeviceClassExtendedInformation*>(refused.Data()));
		if (!Check(result == Pad::PAD_ERROR_INVALID_HANDLE && static_cast<uint32_t>(result) == INVALID_HANDLE_BITS &&
		               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
		           "rejected device-class handle did not return the measured refusal")) return false;
		if (!Check(refused.bytes == refused_expected,
		           "rejected device-class handle did not store exactly the default record")) return false;
	}

	// A null output keeps the family no-write rule (unmeasured on console).
	return Rejected(Operation::DeviceClass, INVALID_HANDLES.front(), nullptr, 1, "null device-class output");
}

bool TestSpecialPortReads()
{
	// Measured console profile (oracle LMNQ92814): both reads of the
	// device-less special port publish exactly the 120-byte default record;
	// Read returns the delivered record count (always one for every count in
	// 1..64), ReadState returns success, and every other count is refused
	// with the measured invalid-argument error and no writes. A refused
	// read-state handle stores the same default record before the shared
	// invalid-handle error. Bytes outside the window keep their canaries.
	constexpr uint32_t INVALID_ARGUMENT_BITS = 0x80920001u;
	const auto record = DeviceLessRecord();

	auto verify = [&](const Storage& storage) {
		for (std::size_t i = 0; i < storage.bytes.size(); ++i)
		{
			const bool inside = i >= 64 && i < 64 + record.size();
			const uint8_t expected = inside ? record[i - 64] : 0xa7;
			if (storage.bytes[i] != expected) return false;
		}
		return true;
	};

	for (int count : {1, 2, 64})
	{
		Storage output;
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadRead(Pad::PAD_SPECIAL_PORT_HANDLE, reinterpret_cast<Pad::PadData*>(output.Data()), count);
		if (!Check(result == 1 && *Posix::GetErrorAddr() == ERRNO_SENTINEL, "special read result changed")) return false;
		if (!Check(verify(output), "special read did not store exactly the default record")) return false;
	}
	{
		Storage output;
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadReadState(Pad::PAD_SPECIAL_PORT_HANDLE, reinterpret_cast<Pad::PadData*>(output.Data()));
		if (!Check(result == 0 && *Posix::GetErrorAddr() == ERRNO_SENTINEL, "special read-state result changed")) return false;
		if (!Check(verify(output), "special read-state did not store exactly the default record")) return false;
	}
	for (int count : INVALID_COUNTS)
	{
		Storage output;
		const auto before = output.bytes;
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadRead(Pad::PAD_SPECIAL_PORT_HANDLE, reinterpret_cast<Pad::PadData*>(output.Data()), count);
		if (!Check(result == Pad::PAD_ERROR_INVALID_ARGUMENT && static_cast<uint32_t>(result) == INVALID_ARGUMENT_BITS &&
		               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
		           "special read accepted an out-of-range count")) return false;
		if (!Check(output.bytes == before, "refused special read changed the output")) return false;
	}
	for (int handle : INVALID_HANDLES)
	{
		Storage output;
		*Posix::GetErrorAddr() = ERRNO_SENTINEL;
		const int result = Pad::PadReadState(handle, reinterpret_cast<Pad::PadData*>(output.Data()));
		if (!Check(result == Pad::PAD_ERROR_INVALID_HANDLE && static_cast<uint32_t>(result) == INVALID_HANDLE_BITS &&
		               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
		           "refused read-state handle did not return the measured refusal")) return false;
		if (!Check(verify(output), "refused read-state did not store exactly the default record")) return false;
	}
	// The device-less reads must not consume the queued samples of the
	// modeled standard pad.
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(Pad::VIRTUAL_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS, true);
	Storage special_output;
	if (!Check(Pad::PadRead(Pad::PAD_SPECIAL_PORT_HANDLE, reinterpret_cast<Pad::PadData*>(special_output.Data()), 1) == 1,
	           "special read failed with queued input")) return false;
	Storage sample;
	if (!Accepted(Operation::Read, sample.Data(), 1, 1) ||
	    !Check(sample.Buttons() == Pad::PAD_BUTTON_CROSS, "special read consumed the queued press")) return false;
	Pad::ControllerResetForNextSession();
	return true;
}

bool TestQueuedSample(Operation invalid_operation, int handle, int invalid_count, bool buffered)
{
	// Existing synthetic input only: no SDL/device backend, hot-plug or mapping.
	// Reset is fixture isolation, not a new lifecycle contract under test.
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(Pad::VIRTUAL_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS, true);
	Pad::ControllerButton(Pad::VIRTUAL_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS, false);
	Storage rejected_output;
	const auto before = rejected_output.bytes;
	if (!Rejected(invalid_operation, handle, rejected_output.Data(), invalid_count, "queued input")) return false;
	if (invalid_operation == Operation::DeviceClass)
	{
		// The measured device-class refusal still stores its default 20-byte
		// record; every other rejected operation must leave the output alone.
		auto expected = before;
		std::fill(expected.begin() + 64, expected.begin() + 84, uint8_t {0});
		if (!Check(rejected_output.bytes == expected, "rejected device-class access did not store the default record"))
			return false;
	}
	else if (invalid_operation == Operation::ReadState)
	{
		// The measured read-state refusal stores the default device-less
		// record (oracle LMNQ92814).
		auto expected = before;
		const auto record = DeviceLessRecord();
		std::copy(record.begin(), record.end(), expected.begin() + 64);
		if (!Check(rejected_output.bytes == expected, "rejected read-state access did not store the default record"))
			return false;
	}
	else if (!Check(rejected_output.bytes == before, "rejected queue access changed output")) return false;
	Storage sample;
	const Operation read = buffered ? Operation::Read : Operation::ReadState;
	const int expected_result = buffered ? 1 : 0;
	if (!Accepted(read, sample.Data(), expected_result) ||
	    !Check(sample.Buttons() == Pad::PAD_BUTTON_CROSS, "invalid handle consumed queued DOWN") ||
	    !Accepted(read, sample.Data(), expected_result) ||
	    !Check(sample.Buttons() == 0, "invalid handle consumed queued UP")) return false;
	if (buffered)
	{
		const auto last_sample = sample.bytes;
		return Accepted(Operation::Read, sample.Data(), 0) &&
		       Check(sample.bytes == last_sample, "empty valid Read changed output");
	}
	return Accepted(Operation::ReadState, sample.Data()) && Check(sample.Buttons() == 0, "released state was not retained");
}

bool TestQueues()
{
	for (bool buffered : {false, true})
	{
		for (int handle : INVALID_HANDLES)
		{
			for (Operation operation : OPERATIONS)
			{
				if (!TestQueuedSample(operation, handle, 1, buffered)) return false;
			}
			for (int count : INVALID_COUNTS)
			{
				if (!TestQueuedSample(Operation::Read, handle, count, buffered)) return false;
			}
		}
	}
	return true;
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Vm::Init();
	Lumen::Loader::Timer::TimerSubsystem::Instance()->Init(nullptr);
	NoAccessMapping inaccessible;
	if (!Check(inaccessible.Valid(), "NoAccess mapping allocation failed") || !TestBindings()) return 1;
	std::puts("PadHandleTest: checking rejection before Controller initialization");
	std::fflush(stdout);
	if (!TestInvalidMatrix(inaccessible.Data(), "before initialization")) return 1;

	Pad::ControllerSubsystem::Instance()->Init(nullptr);
	*Posix::GetErrorAddr() = ERRNO_SENTINEL;
	if (!Check(Pad::PadInit() == 0 && *Posix::GetErrorAddr() == ERRNO_SENTINEL, "valid PadInit changed") ||
	    !Check(Pad::PadOpen(1, Pad::PAD_PORT_TYPE_STANDARD, 0, nullptr) == Pad::PAD_STANDARD_PORT_HANDLE &&
	               *Posix::GetErrorAddr() == ERRNO_SENTINEL,
	           "valid PadOpen changed")) return 1;
	Storage info_before;
	Storage info_after;
	if (!Accepted(Operation::Information, info_before.Data()) || !TestInvalidMatrix(inaccessible.Data(), "after initialization") ||
	    !Accepted(Operation::Information, info_after.Data()) ||
	    !Check(info_before.bytes == info_after.bytes, "invalid operations changed valid controller information") ||
	    !TestPadOpenDomain() || !TestSpecialPortInformation() || !TestValidControls() || !TestDeviceClassInformation() ||
	    !TestSpecialPortReads() || !TestQueues()) return 1;
	std::puts("PadHandleTest: PASS");
	return 0;
}
