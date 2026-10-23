// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/ime_bridge.h"
#include "lumen/succi_benchmark.h"
#include "lumen/virtual_input.h"
#include "common/singleton.h"
#include "core/user_settings.h"
#include "input/controller.h"
#include <SDL3/SDL_events.h>
#include <algorithm>
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
std::atomic_uint64_t gGuestFlipCount {0};
// Protected by the IME publication mutex. Activity replacement forces a new
// publication even when the host editor remains the same session.
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
    if (count > static_cast<jsize>(Lumen::Gen4::Ime::IME_DIALOG_MAX_TEXT_LENGTH) ||
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

uint32_t MapVirtualButton(jint button) {
    constexpr uint32_t values[] = {0x10, 0x20, 0x40, 0x80, 0x400, 0x800,
        0x1000, 0x2000, 0x4000, 0x8000, 2, 4, 8, 0x100000};
    return button >= 0 && button < 14 ? values[button] : 0;
}
Input::GameController* Controller() {
    return (*Common::Singleton<Input::GameControllers>::Instance())[0];
}
Lumen::Gen4::VirtualInput::Queue gVirtualInput;
void WakeVirtualInput() {
    static const Uint32 type = SDL_RegisterEvents(1);
    if (!type) return;
    SDL_Event event{}; event.type = type;
    SDL_PushEvent(&event);
}
void ResetVirtualControllerIfReady() {
    if (!gRuntimeInputReady.load(std::memory_order_acquire)) return;
    gVirtualInput.Reset();
    WakeVirtualInput();
}
bool RequestExit() {
    SDL_Event event{}; event.type = SDL_EVENT_QUIT;
    return SDL_PushEvent(&event);
}

} // namespace

extern "C" void LumenGen4OnRuntimeReady(bool ready) {
    if (!ready) ResetVirtualControllerIfReady();
    gRuntimeInputReady.store(ready, std::memory_order_release);
    CallRuntimeInputReady(ready);
    CallRuntimeProgress(ready ? 1000 : 0, ready ? "PS4 runtime running" : "PS4 runtime stopped");
    if (ready && gRuntimeExitRequested.load(std::memory_order_acquire)) RequestExit();
}
extern "C" void LumenGen4PumpVirtualInput() {
    // SDL owns controller mutation. JNI callbacks only publish owned state and
    // wake its event loop; guest reads never race a Java writer.
    for (const auto& pending : gVirtualInput.Drain()) {
        Controller()->ApplyRemoteState(
            static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(pending.buttons),
            pending.axes, false, 0, 0);
    }
}
extern "C" void LumenGen4OnControllerState(bool connected) {
    // Called by SDL after its controller inventory update. Release virtual
    // state before a physical pad starts delivering input, and restore player
    // one when the last physical device is unplugged.
    gPhysicalControllerConnected.store(connected, std::memory_order_release);
    gVirtualInput.SetEnabled(!connected);
    LumenGen4PumpVirtualInput();
    if (!connected) {
        if (auto* user = UserManagement.GetUserByPlayerIndex(1)) {
            Controller()->user_id = user->user_id;
            Controller()->ConnectController(nullptr);
            UserManagement.LoginUser(user, 1);
        }
    }
    gControllerStateKnown.store(true, std::memory_order_release);
    CallControllerState(connected, true);
}
extern "C" void LumenGen4OnPresentedFrame() { gPresentedFrameCount.fetch_add(1, std::memory_order_release); }
extern "C" void LumenGen4OnGuestFlip() { gGuestFlipCount.fetch_add(1, std::memory_order_release); }
extern "C" bool LumenGen4ShouldExit() { return gRuntimeExitRequested.load(std::memory_order_acquire); }

namespace Lumen::Gen4::Ime {

void SyncAndroidImeDialog()
{
    static std::mutex publish_mutex;
    std::lock_guard publish_lock(publish_mutex);
    // Sample after acquiring the publication lock. A delayed Republish must
    // never reopen a dialog that a newer Close has already dismissed.
    HostSnapshot current;
    { auto& state = State(); std::lock_guard lock(state.mutex); current = state.snapshot; }
    const auto* snapshot = current.generation ? &current : nullptr;
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

} // namespace Lumen::Gen4::Ime

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
    Lumen::Gen4::Ime::Republish();
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
        Lumen::Gen4::Ime::HostReplaceText(
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
        Lumen::Gen4::Ime::HostReplaceText(
            static_cast<uint64_t>(generation), text, static_cast<uint32_t>(cursor)) &&
        Lumen::Gen4::Ime::HostAccept(static_cast<uint64_t>(generation));
    ClearPendingJavaException(environment, "ime-accept");
    return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeImeDialogCancel(
    JNIEnv* environment, jobject activity, jlong generation)
{
    if (IsRuntimeActivity(environment, activity))
    {
        Lumen::Gen4::Ime::HostCancel(static_cast<uint64_t>(generation));
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
    if (!RequestExit())
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
    if (gPhysicalControllerConnected.load(std::memory_order_acquire)) return;
    gVirtualInput.Button(mapped_button, pressed == JNI_TRUE);
    WakeVirtualInput();
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeVirtualAxis(
    JNIEnv*, jobject, jint axis, jint value)
{
    if (!gRuntimeInputReady.load(std::memory_order_acquire))
    {
        return;
    }
    if (axis < 0 || axis >= 6) return;
    if (gPhysicalControllerConnected.load(std::memory_order_acquire)) return;
    gVirtualInput.Axis(static_cast<unsigned>(axis), value);
    WakeVirtualInput();
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

extern "C" JNIEXPORT jlong JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeGetGuestFlipCount(JNIEnv*, jobject)
{
    return static_cast<jlong>(gGuestFlipCount.load(std::memory_order_acquire));
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeGetSucciBenchmarkStatus(JNIEnv* env, jobject) {
    const auto json = Lumen::Gen4::SucciBenchmark::StatusJson();
    return env->NewStringUTF(json.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeCancelSucciBenchmark(JNIEnv*, jobject) {
    Lumen::Gen4::SucciBenchmark::Cancel();
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_RuntimeActivity_nativeSetSucciDemoActive(JNIEnv*, jobject,
                                                                    jboolean active) {
    Lumen::Gen4::SucciBenchmark::SetActive(active == JNI_TRUE);
}
