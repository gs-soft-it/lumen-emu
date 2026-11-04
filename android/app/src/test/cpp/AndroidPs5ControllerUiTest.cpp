// SPDX-License-Identifier: GPL-2.0-only
// Host harness for the complete production AndroidPs5RuntimeUi.cpp and the
// unchanged GameEventController body extracted from each runtime variant.
// JNI uses the JDK's real ABI; SDL/devices and unrelated guest services are doubles.
#include "AndroidPs5RuntimeUi.h"
#include "libs/controller.h"
#include <jni.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

#define REQUIRE(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

extern "C" {
jboolean Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(JNIEnv*, jobject);
void Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeDetachImeUi(JNIEnv*, jobject);
void Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualButton(JNIEnv*, jobject, jint, jboolean);
void Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualAxis(JNIEnv*, jobject, jint, jint);
void Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeResetVirtualController(JNIEnv*, jobject);
}

namespace {
struct Notification { jobject activity; bool connected; bool notify; };
std::vector<Notification> notifications;
std::function<void(jobject)> on_callback;
_jclass activity_class;
int method_token;
int weak_refs = 0, local_refs = 0;
bool pending = false, fail_weak = false, fail_local = false, fail_callback = false;
bool environment_available = true;
unsigned virtual_resets = 0, virtual_buttons = 0, virtual_axes = 0;
uint32_t last_button = 0;
Libs::Controller::Axis last_axis = Libs::Controller::Axis::AxisMax;
int last_axis_value = -1;
bool last_pressed = false;
std::set<int> pads;
// The keyboard/touch host source is negative, not a physical gamepad.
int idle_controller_id = Libs::Controller::HOST_INPUT_CONTROLLER_ID;

jboolean JNICALL ExceptionCheck(JNIEnv*) { return pending ? JNI_TRUE : JNI_FALSE; }
void JNICALL ExceptionClear(JNIEnv*) { pending = false; }
jweak JNICALL NewWeakGlobalRef(JNIEnv*, jobject object) {
    if (fail_weak) { fail_weak = false; pending = true; return nullptr; }
    ++weak_refs;
    return object;
}
void JNICALL DeleteWeakGlobalRef(JNIEnv*, jweak object) { REQUIRE(object); REQUIRE(--weak_refs >= 0); }
jobject JNICALL NewLocalRef(JNIEnv*, jobject object) {
    if (fail_local) { fail_local = false; pending = true; return nullptr; }
    if (object) ++local_refs;
    return object;
}
void JNICALL DeleteLocalRef(JNIEnv*, jobject object) { REQUIRE(object); REQUIRE(--local_refs >= 0); }
jboolean JNICALL IsSameObject(JNIEnv*, jobject a, jobject b) { return a == b ? JNI_TRUE : JNI_FALSE; }
jclass JNICALL GetObjectClass(JNIEnv*, jobject object) {
    REQUIRE(object);
    ++local_refs;
    return &activity_class;
}
jmethodID JNICALL GetMethodID(JNIEnv*, jclass klass, const char* name, const char* signature) {
    REQUIRE(klass == &activity_class);
    REQUIRE(std::strcmp(name, "onNativeControllerState") == 0);
    REQUIRE(std::strcmp(signature, "(ZZ)V") == 0);
    return reinterpret_cast<jmethodID>(&method_token);
}
void JNICALL CallVoidMethodV(JNIEnv*, jobject object, jmethodID method, va_list arguments) {
    REQUIRE(!pending);
    REQUIRE(method == reinterpret_cast<jmethodID>(&method_token));
    const int connected = va_arg(arguments, int), notify = va_arg(arguments, int);
    REQUIRE((connected == JNI_FALSE || connected == JNI_TRUE) &&
            (notify == JNI_FALSE || notify == JNI_TRUE));
    if (fail_callback) { fail_callback = false; pending = true; return; }
    notifications.push_back({object, connected == JNI_TRUE, notify == JNI_TRUE});
    if (on_callback) on_callback(object);
}
JNINativeInterface_ functions = [] {
    JNINativeInterface_ table{};
    table.ExceptionCheck = ExceptionCheck;
    table.ExceptionClear = ExceptionClear;
    table.NewWeakGlobalRef = NewWeakGlobalRef;
    table.DeleteWeakGlobalRef = DeleteWeakGlobalRef;
    table.NewLocalRef = NewLocalRef;
    table.DeleteLocalRef = DeleteLocalRef;
    table.IsSameObject = IsSameObject;
    table.GetObjectClass = GetObjectClass;
    table.GetMethodID = GetMethodID;
    table.CallVoidMethodV = CallVoidMethodV;
    return table;
}();
JNIEnv environment{&functions};

void ExpectNone() { REQUIRE(notifications.empty()); REQUIRE(!pending); REQUIRE(local_refs == 0); }
void ExpectOnly(jobject object, bool connected, bool notify) {
    REQUIRE(notifications.size() == 1);
    REQUIRE(notifications[0].activity == object);
    REQUIRE(notifications[0].connected == connected);
    REQUIRE(notifications[0].notify == notify);
    notifications.clear();
    ExpectNone();
}
void Attach(jobject object) {
    REQUIRE(Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(&environment, object) == JNI_TRUE);
    REQUIRE(weak_refs == 1);
}
void Detach(jobject object) {
    Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeDetachImeUi(&environment, object);
}
void Touch(jobject object) {
    Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualButton(&environment, object, 8, JNI_TRUE);
    Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualAxis(&environment, object, 0, 255);
}
void ExpectTouchBlocked(jobject object) {
    const auto buttons = virtual_buttons, axes = virtual_axes;
    Touch(object);
    REQUIRE(virtual_buttons == buttons && virtual_axes == axes);
}
void ExpectTouchAccepted(jobject object) {
    const auto buttons = virtual_buttons, axes = virtual_axes;
    Touch(object);
    REQUIRE(virtual_buttons == buttons + 1 && virtual_axes == axes + 1);
    REQUIRE(last_button == Libs::Controller::PAD_BUTTON_CROSS && last_pressed);
    REQUIRE(last_axis == Libs::Controller::Axis::LeftX && last_axis_value == 255);
}
} // namespace

void* SDL_GetAndroidJNIEnv() { return environment_available ? &environment : nullptr; }
int __android_log_print(int, const char*, const char*, ...) { return 0; }
int __android_log_write(int, const char*, const char*) { return 0; }

namespace Libs::Controller {
void ResetVirtualInputState() { ++virtual_resets; }
void SetVirtualButton(uint32_t button, bool pressed) { ++virtual_buttons; last_button = button; last_pressed = pressed; }
void SetVirtualAxis(Axis axis, int value) { ++virtual_axes; last_axis = axis; last_axis_value = value; }
void ResetInputState() { REQUIRE(false); } // A UI transition must not reset held physical controls.
void Connect(int id) { REQUIRE(id > 0); pads.insert(id); }
void Disconnect(int id) { pads.erase(id); }
int GetActiveControllerId() { return pads.empty() ? idle_controller_id : *pads.begin(); }
void SetButton(int, uint32_t, bool) { REQUIRE(false); }
void SetAxis(int, Axis, int) { REQUIRE(false); }
} // namespace Libs::Controller

// Narrow SDL lifecycle double. Mapping/polling and hardware discovery are not
// reimplemented or qualified by this test.
struct SDL_Gamepad { int id; };
SDL_Gamepad* SDL_OpenGamepad(int id) { static SDL_Gamepad pad; pad.id = id; return &pad; }
SDL_Gamepad* SDL_GetGamepadJoystick(SDL_Gamepad* pad) { return pad; }
int SDL_GetJoystickID(SDL_Gamepad* pad) { return pad->id; }
SDL_Gamepad* SDL_GetGamepadFromID(int id) { return SDL_OpenGamepad(id); }
void SDL_CloseGamepad(SDL_Gamepad*) {}

namespace Libs::Graphics {
uint64_t GetPresentedGuestFrameCount() { return 0; }
bool SystemImeHostReplaceText(uint64_t, std::u16string_view, uint32_t) { REQUIRE(false); return false; }
bool SystemImeHostAccept(uint64_t) { REQUIRE(false); return false; }
bool SystemImeHostCancel(uint64_t) { REQUIRE(false); return false; }
struct EventController {
    bool remapped = false, added = false, removed = false, down = false, up = false, axis = false;
    int id = 0, button = 0, axis_id = 0, axis_value = 0;
};
uint32_t ControllerButtonToPadButton(int) { REQUIRE(false); return 0; }
Controller::Axis ControllerAxisFromSdl(int) { REQUIRE(false); return Controller::Axis::AxisMax; }
int ControllerAxisValueFromSdl(int, int) { REQUIRE(false); return 0; }
#define EXIT_NOT_IMPLEMENTED(expression) REQUIRE(!(expression))
#include "AndroidPs5ControllerUiWindow.inc"
#undef EXIT_NOT_IMPLEMENTED
void AddPad(int id) { EventController event; event.id = id; event.added = true; GameEventController(event); }
void RemovePad(int id) { EventController event; event.id = id; event.removed = true; GameEventController(event); }
} // namespace Libs::Graphics

int main() {
    using namespace Libs::Graphics;
    _jobject first, second;
    REQUIRE(Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(nullptr, &first) == JNI_FALSE);
    REQUIRE(Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(&environment, nullptr) == JNI_FALSE);
    ExpectNone();

    // A fresh Activity gets a silent false replay; virtual button/axis mapping works.
    Attach(&first); ExpectOnly(&first, false, false);
    ExpectTouchAccepted(&first);
    auto resets = virtual_resets;
    AddPad(10); ExpectOnly(&first, true, true);
    REQUIRE(virtual_resets == resets + 1);
    ExpectTouchBlocked(&first);
    // Duplicate/second gamepads and removal with another pad alive do not toast
    // twice, reveal the virtual pad, or release the remaining physical source.
    resets = virtual_resets;
    AddPad(10); AddPad(20); RemovePad(10); ExpectNone();
    REQUIRE(virtual_resets == resets && Libs::Controller::GetActiveControllerId() == 20);
    ExpectTouchBlocked(&first);
    Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeResetVirtualController(&environment, &first);
    REQUIRE(virtual_resets == resets + 1);
    RemovePad(20); ExpectOnly(&first, false, false);
    ExpectTouchAccepted(&first);
    RemovePad(20); ExpectNone(); // Negative keyboard/touch id is not a gamepad.
    idle_controller_id = 0;
    RemovePad(20); ExpectNone(); // Neither is the no-controller sentinel.
    idle_controller_id = Libs::Controller::HOST_INPUT_CONTROLLER_ID;

    // Replacement/recreation replays the current state without a connection toast.
    AddPad(30); ExpectOnly(&first, true, true);
    Attach(&second); ExpectOnly(&second, true, false);
    Detach(&first); REQUIRE(weak_refs == 1); ExpectNone();
    RemovePad(30); ExpectOnly(&second, false, false);
    ExpectTouchBlocked(&first); ExpectTouchAccepted(&second);
    Detach(&second); REQUIRE(weak_refs == 0);
    ExpectTouchBlocked(&second);
    AddPad(40); ExpectNone();
    Attach(&first); ExpectOnly(&first, true, false);
    Detach(&first); RemovePad(40); ExpectNone();
    Attach(&second); ExpectOnly(&second, false, false);

    // Real JNI callbacks can re-enter native code. A callback made under gUiMutex
    // would deadlock here and the bounded runner would fail.
    on_callback = [](jobject activity) {
        Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeResetVirtualController(&environment, activity);
    };
    resets = virtual_resets;
    AddPad(50); ExpectOnly(&second, true, true);
    REQUIRE(virtual_resets == resets + 2);
    Attach(&first); ExpectOnly(&first, true, false);
    on_callback = {};
    RemovePad(50); ExpectOnly(&first, false, false);

    // An SDL thread without JNI still records state for the next attach.
    environment_available = false;
    AddPad(60); ExpectNone(); ExpectTouchBlocked(&first);
    environment_available = true;
    Attach(&second); ExpectOnly(&second, true, false);
    RemovePad(60); ExpectOnly(&second, false, false);

    // Failed reference allocation/callbacks clear exceptions and leak no refs.
    // The next attachment replays cached truth, without a spurious toast.
    fail_weak = true;
    REQUIRE(Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(&environment, &first) == JNI_FALSE);
    REQUIRE(weak_refs == 1); ExpectNone(); ExpectTouchAccepted(&second);
    fail_local = true;
    AddPad(70); ExpectNone(); ExpectTouchBlocked(&second);
    Attach(&first); ExpectOnly(&first, true, false);
    fail_callback = true;
    RemovePad(70); ExpectNone(); ExpectTouchAccepted(&first);
    Attach(&second); ExpectOnly(&second, false, false);
    Detach(&second); ExpectNone();
    REQUIRE(weak_refs == 0 && pads.empty());
    std::puts("PASS: real PS5 UI bridge + runtime hotplug body; connect/disconnect, multipad, "
              "negative/zero host ids, silent attach replay, duplicate suppression, detach/replacement, "
              "virtual-only reset, touch gating, JNI reentrancy/errors and reference cleanup");
}
