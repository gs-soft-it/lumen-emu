#include "Emulator/Graphics/WindowAndroid.h"

#include "Emulator/Controller.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Libs/ImeDialogAndroidUi.h"

#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <jni.h>
#include <mutex>
#include <string>

namespace {

constexpr const char* LOG_TAG = "LumenAndroidRuntime";

std::mutex       gUiMutex;
JavaVM*          gJavaVm = nullptr;
jweak            gRuntimeActivity = nullptr;
std::atomic_bool gRuntimeInputReady {false};
std::atomic_bool gRuntimeExitRequested {false};
std::atomic_bool gControllerStateKnown {false};
std::atomic_bool gPhysicalControllerConnected {false};
std::atomic_uint64_t gPresentedFrameCount {0};
// Only the SDL event thread publishes dialog generations. Activity replacement
// forces a new publication even when the host editor remains the same session.
uint64_t gImePublishedGeneration = 0;
std::atomic_uint64_t gRuntimeActivityRevision {0};
uint64_t gImePublishedActivityRevision = 0;

class ScopedJniEnvironment final
{
public:
    ScopedJniEnvironment()
    {
        JavaVM* vm = nullptr;
        {
            std::lock_guard<std::mutex> lock(gUiMutex);
            vm = gJavaVm;
        }
        if (vm == nullptr)
        {
            return;
        }

        void* raw_environment = nullptr;
        const jint state = vm->GetEnv(&raw_environment, JNI_VERSION_1_6);
        if (state == JNI_OK)
        {
            environment_ = static_cast<JNIEnv*>(raw_environment);
        } else if (state == JNI_EDETACHED &&
                   vm->AttachCurrentThread(&environment_, nullptr) == JNI_OK)
        {
            attached_ = true;
        }
        vm_ = vm;
    }

    ~ScopedJniEnvironment()
    {
        if (attached_ && vm_ != nullptr)
        {
            vm_->DetachCurrentThread();
        }
    }

    JNIEnv* Get() const { return environment_; }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* environment_ = nullptr;
    bool    attached_ = false;
};

jobject AcquireRuntimeActivity(JNIEnv* environment)
{
    if (environment == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    return gRuntimeActivity != nullptr
        ? environment->NewLocalRef(gRuntimeActivity)
        : nullptr;
}

bool IsRuntimeActivity(JNIEnv* environment, jobject activity)
{
    std::lock_guard<std::mutex> lock(gUiMutex);
    return environment != nullptr && activity != nullptr && gRuntimeActivity != nullptr &&
           environment->IsSameObject(gRuntimeActivity, activity);
}

bool ReadImeText(JNIEnv* environment, jstring value, jint cursor,
                 std::u16string* text)
{
    if (environment == nullptr || value == nullptr || text == nullptr || cursor < 0)
    {
        return false;
    }
    const jsize count = environment->GetStringLength(value);
    if (count > static_cast<jsize>(Lumen::Libs::Dialog::ImeDialog::IME_DIALOG_MAX_TEXT_LENGTH) ||
        cursor > count)
    {
        return false;
    }
    const jchar* data = environment->GetStringChars(value, nullptr);
    if (data == nullptr)
    {
        return false;
    }
    static_assert(sizeof(jchar) == sizeof(char16_t));
    text->assign(reinterpret_cast<const char16_t*>(data), static_cast<size_t>(count));
    environment->ReleaseStringChars(value, data);
    return true;
}

void ClearPendingJavaException(JNIEnv* environment, const char* callback)
{
    if (environment == nullptr || !environment->ExceptionCheck())
    {
        return;
    }
    environment->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "LUMEN_ANDROID_RUNTIME_UI_V1 FAIL callback=%s",
                        callback != nullptr ? callback : "unknown");
}

void CallControllerState(bool connected, bool notify)
{
    ScopedJniEnvironment scoped;
    JNIEnv* environment = scoped.Get();
    jobject activity = AcquireRuntimeActivity(environment);
    if (activity == nullptr)
    {
        return;
    }
    jclass activity_class = environment->GetObjectClass(activity);
    jmethodID callback = activity_class != nullptr
        ? environment->GetMethodID(activity_class, "onNativeControllerState", "(ZZ)V")
        : nullptr;
    if (callback != nullptr)
    {
        environment->CallVoidMethod(activity, callback,
                                    connected ? JNI_TRUE : JNI_FALSE,
                                    notify ? JNI_TRUE : JNI_FALSE);
    }
    ClearPendingJavaException(environment, "controller-state");
    if (activity_class != nullptr)
    {
        environment->DeleteLocalRef(activity_class);
    }
    environment->DeleteLocalRef(activity);
}

void CallRuntimeInputReady(bool ready)
{
    ScopedJniEnvironment scoped;
    JNIEnv* environment = scoped.Get();
    jobject activity = AcquireRuntimeActivity(environment);
    if (activity == nullptr)
    {
        return;
    }
    jclass activity_class = environment->GetObjectClass(activity);
    jmethodID callback = activity_class != nullptr
        ? environment->GetMethodID(activity_class, "onNativeRuntimeInputReady", "(Z)V")
        : nullptr;
    if (callback != nullptr)
    {
        environment->CallVoidMethod(activity, callback,
                                    ready ? JNI_TRUE : JNI_FALSE);
    }
    ClearPendingJavaException(environment, "runtime-input-ready");
    if (activity_class != nullptr)
    {
        environment->DeleteLocalRef(activity_class);
    }
    environment->DeleteLocalRef(activity);
}

void CallRuntimeProgress(uint32_t progress_permille, const char* message)
{
    ScopedJniEnvironment scoped;
    JNIEnv* environment = scoped.Get();
    jobject activity = AcquireRuntimeActivity(environment);
    if (activity == nullptr)
    {
        return;
    }
    jclass activity_class = environment->GetObjectClass(activity);
    jmethodID callback = activity_class != nullptr
        ? environment->GetMethodID(activity_class, "onNativeRuntimeProgress",
                                   "(ILjava/lang/String;)V")
        : nullptr;
    jstring java_message = environment->NewStringUTF(
        message != nullptr && message[0] != '\0' ? message : "Runtime activity");
    if (callback != nullptr && java_message != nullptr)
    {
        environment->CallVoidMethod(activity, callback,
                                    static_cast<jint>(progress_permille),
                                    java_message);
    }
    ClearPendingJavaException(environment, "runtime-progress");
    if (java_message != nullptr)
    {
        environment->DeleteLocalRef(java_message);
    }
    if (activity_class != nullptr)
    {
        environment->DeleteLocalRef(activity_class);
    }
    environment->DeleteLocalRef(activity);
}

uint32_t MapVirtualButton(jint button)
{
    using namespace Lumen::Libs::Controller;
    switch (button)
    {
        case 0: return PAD_BUTTON_UP;
        case 1: return PAD_BUTTON_RIGHT;
        case 2: return PAD_BUTTON_DOWN;
        case 3: return PAD_BUTTON_LEFT;
        case 4: return PAD_BUTTON_L1;
        case 5: return PAD_BUTTON_R1;
        case 6: return PAD_BUTTON_TRIANGLE;
        case 7: return PAD_BUTTON_CIRCLE;
        case 8: return PAD_BUTTON_CROSS;
        case 9: return PAD_BUTTON_SQUARE;
        case 10: return PAD_BUTTON_L3;
        case 11: return PAD_BUTTON_R3;
        case 12: return PAD_BUTTON_OPTIONS;
        case 13: return PAD_BUTTON_TOUCH_PAD;
        default: return 0;
    }
}

Lumen::Libs::Controller::Axis MapVirtualAxis(jint axis)
{
    using Lumen::Libs::Controller::Axis;
    switch (axis)
    {
        case 0: return Axis::LeftX;
        case 1: return Axis::LeftY;
        case 2: return Axis::RightX;
        case 3: return Axis::RightY;
        case 4: return Axis::TriggerLeft;
        case 5: return Axis::TriggerRight;
        default: return Axis::AxisMax;
    }
}

void ResetVirtualControllerIfReady()
{
    if (!gRuntimeInputReady.load(std::memory_order_acquire))
    {
        return;
    }
    using namespace Lumen::Libs::Controller;
    ControllerEnsureVirtualConnected();
    constexpr uint32_t buttons[] = {
        PAD_BUTTON_UP, PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT,
        PAD_BUTTON_L1, PAD_BUTTON_R1, PAD_BUTTON_TRIANGLE, PAD_BUTTON_CIRCLE,
        PAD_BUTTON_CROSS, PAD_BUTTON_SQUARE, PAD_BUTTON_L3, PAD_BUTTON_R3,
        PAD_BUTTON_OPTIONS, PAD_BUTTON_TOUCH_PAD,
    };
    for (uint32_t button : buttons)
    {
        ControllerButton(VIRTUAL_CONTROLLER_ID, button, false);
    }
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::LeftX, 128);
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::LeftY, 128);
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::RightX, 128);
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::RightY, 128);
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::TriggerLeft, 0);
    ControllerAxis(VIRTUAL_CONTROLLER_ID, Axis::TriggerRight, 0);
}

} // namespace

namespace Lumen::Libs::Graphics {

void WindowAndroidRuntimeInputReady(bool ready)
{
    if (!ready)
    {
        ResetVirtualControllerIfReady();
    }
    gRuntimeInputReady.store(ready, std::memory_order_release);
    CallRuntimeInputReady(ready);
    if (ready && gRuntimeExitRequested.load(std::memory_order_acquire))
    {
        (void)Lumen::Loader::RequestGuestSessionExit();
    }
}

void WindowAndroidControllerStateChanged(bool connected, bool notify)
{
    gPhysicalControllerConnected.store(connected, std::memory_order_release);
    gControllerStateKnown.store(true, std::memory_order_release);
    CallControllerState(connected, notify);
}

void WindowAndroidDidPresentFrame(uint64_t frame_number)
{
    gPresentedFrameCount.store(frame_number, std::memory_order_release);
}

void WindowAndroidRuntimeProgress(uint32_t progress_permille, const char* message)
{
    CallRuntimeProgress(progress_permille > 1000u ? 1000u : progress_permille,
                        message);
}

} // namespace Lumen::Libs::Graphics

namespace Lumen::Libs::Dialog::ImeDialog {

void SyncAndroidImeDialog(const HostSnapshot* snapshot)
{
    const uint64_t generation = snapshot != nullptr ? snapshot->generation : 0;
    const uint64_t activity_revision = gRuntimeActivityRevision.load(std::memory_order_acquire);
    if (generation == gImePublishedGeneration &&
        activity_revision == gImePublishedActivityRevision)
    {
        return;
    }
    ScopedJniEnvironment scoped;
    JNIEnv* environment = scoped.Get();
    jobject activity = AcquireRuntimeActivity(environment);
    if (activity == nullptr)
    {
        return;
    }
    jclass activity_class = environment->GetObjectClass(activity);
    jmethodID callback = activity_class != nullptr
        ? environment->GetMethodID(activity_class, "onNativeImeDialog",
                                   "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;IIIII)V")
        : nullptr;
    const auto make_string = [environment](const std::u16string& value) {
        return environment->NewString(reinterpret_cast<const jchar*>(value.data()),
                                      static_cast<jsize>(value.size()));
    };
    jstring text = nullptr;
    jstring title = nullptr;
    jstring placeholder = nullptr;
    if (callback != nullptr && !environment->ExceptionCheck() && snapshot != nullptr)
    {
        text = make_string(snapshot->text);
        if (text != nullptr && !environment->ExceptionCheck()) title = make_string(snapshot->title);
        if (title != nullptr && !environment->ExceptionCheck()) placeholder = make_string(snapshot->placeholder);
    }
    if (callback != nullptr && !environment->ExceptionCheck() &&
        (snapshot == nullptr || (text != nullptr && title != nullptr && placeholder != nullptr)))
    {
        environment->CallVoidMethod(activity, callback, static_cast<jlong>(generation),
                                    text, title, placeholder,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->cursor) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->max_text_length) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->type) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->option) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->enter_label) : 0);
        if (!environment->ExceptionCheck())
        {
            gImePublishedGeneration = generation;
            gImePublishedActivityRevision = activity_revision;
        }
    }
    ClearPendingJavaException(environment, "ime-dialog");
    if (text != nullptr) environment->DeleteLocalRef(text);
    if (title != nullptr) environment->DeleteLocalRef(title);
    if (placeholder != nullptr) environment->DeleteLocalRef(placeholder);
    if (activity_class != nullptr) environment->DeleteLocalRef(activity_class);
    environment->DeleteLocalRef(activity);
}

} // namespace Lumen::Libs::Dialog::ImeDialog

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeAttachRuntimeUi(
    JNIEnv* environment, jobject activity)
{
    if (environment == nullptr || activity == nullptr)
    {
        return;
    }
    JavaVM* vm = nullptr;
    if (environment->GetJavaVM(&vm) != JNI_OK || vm == nullptr)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gUiMutex);
        if (gRuntimeActivity != nullptr)
        {
            environment->DeleteWeakGlobalRef(gRuntimeActivity);
        }
        gJavaVm = vm;
        gRuntimeActivity = environment->NewWeakGlobalRef(activity);
        gRuntimeActivityRevision.fetch_add(1, std::memory_order_release);
    }
    CallRuntimeInputReady(gRuntimeInputReady.load(std::memory_order_acquire));
    if (gControllerStateKnown.load(std::memory_order_acquire))
    {
        CallControllerState(
            gPhysicalControllerConnected.load(std::memory_order_acquire), false);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeDetachRuntimeUi(
    JNIEnv* environment, jobject activity)
{
    if (environment == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (gRuntimeActivity != nullptr &&
        (activity == nullptr || environment->IsSameObject(gRuntimeActivity, activity)))
    {
        environment->DeleteWeakGlobalRef(gRuntimeActivity);
        gRuntimeActivity = nullptr;
        gRuntimeActivityRevision.fetch_add(1, std::memory_order_release);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeImeDialogUpdate(
    JNIEnv* environment, jobject activity, jlong generation, jstring value, jint cursor)
{
    std::u16string text;
    const bool accepted = IsRuntimeActivity(environment, activity) &&
        ReadImeText(environment, value, cursor, &text) &&
        Lumen::Libs::Dialog::ImeDialog::HostReplaceText(
            static_cast<uint64_t>(generation), text, static_cast<uint32_t>(cursor));
    ClearPendingJavaException(environment, "ime-update");
    return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeImeDialogAccept(
    JNIEnv* environment, jobject activity, jlong generation, jstring value, jint cursor)
{
    // A final synchronous replacement includes the last composing edit even if
    // its posted TextWatcher update has not run before the button callback.
    std::u16string text;
    const bool accepted = IsRuntimeActivity(environment, activity) &&
        ReadImeText(environment, value, cursor, &text) &&
        Lumen::Libs::Dialog::ImeDialog::HostReplaceText(
            static_cast<uint64_t>(generation), text, static_cast<uint32_t>(cursor)) &&
        Lumen::Libs::Dialog::ImeDialog::HostAccept(static_cast<uint64_t>(generation));
    ClearPendingJavaException(environment, "ime-accept");
    return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeImeDialogCancel(
    JNIEnv* environment, jobject activity, jlong generation)
{
    if (IsRuntimeActivity(environment, activity))
    {
        Lumen::Libs::Dialog::ImeDialog::HostCancel(static_cast<uint64_t>(generation));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeRequestExit(
    JNIEnv*, jobject)
{
    gRuntimeExitRequested.store(true, std::memory_order_release);
    ResetVirtualControllerIfReady();
    gRuntimeInputReady.store(false, std::memory_order_release);
    CallRuntimeInputReady(false);
    if (!Lumen::Loader::RequestGuestSessionExit())
    {
        __android_log_write(ANDROID_LOG_WARN, LOG_TAG,
                            "LUMEN_ANDROID_RUNTIME_UI_V1 exit-event=not-enqueued stop=latched");
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeVirtualButton(
    JNIEnv*, jobject, jint button, jboolean pressed)
{
    if (!gRuntimeInputReady.load(std::memory_order_acquire))
    {
        return;
    }
    const uint32_t mapped_button = MapVirtualButton(button);
    if (mapped_button == 0)
    {
        return;
    }
    using namespace Lumen::Libs::Controller;
    ControllerEnsureVirtualConnected();
    ControllerButton(VIRTUAL_CONTROLLER_ID, mapped_button, pressed == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeVirtualAxis(
    JNIEnv*, jobject, jint axis, jint value)
{
    if (!gRuntimeInputReady.load(std::memory_order_acquire))
    {
        return;
    }
    using namespace Lumen::Libs::Controller;
    const Axis mapped_axis = MapVirtualAxis(axis);
    if (mapped_axis == Axis::AxisMax)
    {
        return;
    }
    const int bounded_value = value < 0 ? 0 : (value > 255 ? 255 : value);
    ControllerEnsureVirtualConnected();
    ControllerAxis(VIRTUAL_CONTROLLER_ID, mapped_axis, bounded_value);
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeResetVirtualController(
    JNIEnv*, jobject)
{
    ResetVirtualControllerIfReady();
}

extern "C" JNIEXPORT jlong JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeGetPresentedFrameCount(
    JNIEnv*, jobject)
{
    return static_cast<jlong>(
        gPresentedFrameCount.load(std::memory_order_acquire));
}
