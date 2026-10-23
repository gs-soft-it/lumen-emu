#ifndef EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_
#define EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Controller {

LUMEN_SUBSYSTEM_DEFINE(Controller);

constexpr uint32_t PAD_BUTTON_L3        = 0x00000002;
constexpr uint32_t PAD_BUTTON_R3        = 0x00000004;
constexpr uint32_t PAD_BUTTON_OPTIONS   = 0x00000008;
constexpr uint32_t PAD_BUTTON_UP        = 0x00000010;
constexpr uint32_t PAD_BUTTON_RIGHT     = 0x00000020;
constexpr uint32_t PAD_BUTTON_DOWN      = 0x00000040;
constexpr uint32_t PAD_BUTTON_LEFT      = 0x00000080;
constexpr uint32_t PAD_BUTTON_L2        = 0x00000100;
constexpr uint32_t PAD_BUTTON_R2        = 0x00000200;
constexpr uint32_t PAD_BUTTON_L1        = 0x00000400;
constexpr uint32_t PAD_BUTTON_R1        = 0x00000800;
constexpr uint32_t PAD_BUTTON_TRIANGLE  = 0x00001000;
constexpr uint32_t PAD_BUTTON_CIRCLE    = 0x00002000;
constexpr uint32_t PAD_BUTTON_CROSS     = 0x00004000;
constexpr uint32_t PAD_BUTTON_SQUARE    = 0x00008000;
constexpr uint32_t PAD_BUTTON_TOUCH_PAD = 0x00100000;
constexpr int      VIRTUAL_CONTROLLER_ID = 9999;

enum class Axis
{
	LeftX        = 0,
	LeftY        = 1,
	RightX       = 2,
	RightY       = 3,
	TriggerLeft  = 4,
	TriggerRight = 5,

	AxisMax
};

struct PadControllerInformation;
struct PadDeviceClassExtendedInformation;
struct PadData;
struct PadVibrationParam;
struct PadLightBarParam;

// Device class the runtime publishes for its single emulated standard pad in
// both the controller information and the extended information. The console
// reports class 0 (the Standard member of the public device-class
// enumeration) for a connected standard pad in the controller information
// (oracle LMNQ92801) and in the extended information (oracle LMNQ92802).
constexpr int PAD_MODELED_DEVICE_CLASS = 0;

// Device class the console publishes in the extended information of the
// device-less special port (oracle LMNQ92802): -1, the Invalid member of the
// public device-class enumeration, inside the same zeroed 16-byte area the
// standard port receives. The controller information of the same port still
// reports class 0 (oracle LMNQ92801).
constexpr int PAD_DEVICE_CLASS_INVALID = -1;

// Size of the extended-information record the console stores, measured on
// both modeled ports and on refused handles (oracle LMNQ92802): a 32-bit
// device class plus a 16-byte area. The local declaration mirrors exactly
// this write window.
constexpr int PAD_DEVICE_CLASS_RECORD_SIZE = 20;

// Write window of the modeled pad record measured on the console (oracle
// LMNQ92814): both reads of the modeled ports store exactly 120 bytes, from
// the live record of the standard pad to the default record of the
// device-less special port.
constexpr int PAD_DATA_RECORD_SIZE = 120;

// Port types of scePadOpen measured on the console (oracle LMNQ92801): the
// standard port (0) and the special port (2) are recognized, every other
// value returns PAD_ERROR_INVALID_ARGUMENT before the user is validated. The
// special port opens with no device attached.
constexpr int PAD_PORT_TYPE_STANDARD = 0;
constexpr int PAD_PORT_TYPE_SPECIAL  = 2;

// Local handle values of the two modeled ports. The console returns opaque
// kernel handles; this local model keeps the historical handle 1 for the
// standard port and uses 2 for the device-less special port. Every other
// value stays PAD_ERROR_INVALID_HANDLE for the handle-taking calls.
constexpr int PAD_STANDARD_PORT_HANDLE = 1;
constexpr int PAD_SPECIAL_PORT_HANDLE  = 2;

inline int controller_get_axis(int min, int max, int value)
{
	int v = (255 * (value - min)) / (max - min);
	return (v < 0 ? 0 : (v > 255 ? 255 : v));
}

void ControllerConnect(int id);
// Idempotently makes the shared keyboard/touch fallback available. Physical
// controllers still retain priority in the controller ordering.
void ControllerEnsureVirtualConnected();
// Clears every queued sample at an iOS repeatable-session boundary while
// preserving the process-lifetime device ordering.
void ControllerResetForNextSession();
void ControllerDisconnect(int id);
void ControllerButton(int id, uint32_t button, bool down);
void ControllerAxis(int id, Axis axis, int value);
void ControllerSetLeftStick(int id, int lx, int ly);

// Bounded, opt-in diagnostics for Options delivery from SDL to the Pad reader.
bool ControllerOptionsTraceEnabled();
void ControllerTraceOptionsEvent(int id, bool down, uint32_t event_ticks, uint32_t current_ticks,
                                 const char* name, const char* path, const char* guid);
bool ControllerTriggerTraceEnabled();
void ControllerTraceTriggerEvent(int id, int axis, int value, uint32_t event_ticks, uint32_t current_ticks);

int LUMEN_SYSV_ABI PadInit();
int LUMEN_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param);
int LUMEN_SYSV_ABI PadSetMotionSensorState(int handle, bool enable);
int LUMEN_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info);
int LUMEN_SYSV_ABI PadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info);
int LUMEN_SYSV_ABI PadReadState(int handle, PadData* data);
int LUMEN_SYSV_ABI PadRead(int handle, PadData* data, int num);
int LUMEN_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param);
int LUMEN_SYSV_ABI PadResetLightBar(int handle);
int LUMEN_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param);

} // namespace Lumen::Libs::Controller

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_ */
