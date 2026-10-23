// SPDX-License-Identifier: GPL-2.0-only
#include "AndroidPs5RuntimeUi.h"

#include "common/renderTiming.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "libs/controller.h"

#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <array>
#include <cstdint>
#include <exception>
#include <jni.h>
#include <mutex>
#include <string>

namespace {

constexpr const char* LOG_TAG = "LumenAndroidPs5";

std::mutex gUiMutex;
jweak      gRuntimeActivity = nullptr;
uint64_t   gRuntimeActivityRevision = 0;
// Only the SDL event thread publishes snapshots. Replacing the Activity forces
// publication even if the guest editor generation has not changed.
uint64_t gPublishedGeneration = 0;
uint64_t gPublishedActivityRevision = 0;

// Ordinals are the Java VirtualControllerView JNI contract, not pad bit masks.
constexpr std::array<uint32_t, 14> VIRTUAL_BUTTONS {
    Libs::Controller::PAD_BUTTON_UP, Libs::Controller::PAD_BUTTON_RIGHT,
    Libs::Controller::PAD_BUTTON_DOWN, Libs::Controller::PAD_BUTTON_LEFT,
    Libs::Controller::PAD_BUTTON_L1, Libs::Controller::PAD_BUTTON_R1,
    Libs::Controller::PAD_BUTTON_TRIANGLE, Libs::Controller::PAD_BUTTON_CIRCLE,
    Libs::Controller::PAD_BUTTON_CROSS, Libs::Controller::PAD_BUTTON_SQUARE,
    Libs::Controller::PAD_BUTTON_L3, Libs::Controller::PAD_BUTTON_R3,
    Libs::Controller::PAD_BUTTON_OPTIONS, Libs::Controller::PAD_BUTTON_TOUCH_PAD
};
constexpr std::array<Libs::Controller::Axis, 6> VIRTUAL_AXES {
    Libs::Controller::Axis::LeftX, Libs::Controller::Axis::LeftY,
    Libs::Controller::Axis::RightX, Libs::Controller::Axis::RightY,
    Libs::Controller::Axis::TriggerLeft, Libs::Controller::Axis::TriggerRight
};

// Call with gUiMutex held so a replaced Activity cannot leave late touch state
// behind after detach. Controller guards its own initialization and lifetime.
bool IsRuntimeActivityLocked(JNIEnv* environment, jobject activity)
{
    return environment != nullptr && activity != nullptr && gRuntimeActivity != nullptr &&
           environment->IsSameObject(gRuntimeActivity, activity);
}

void ClearPendingJavaException(JNIEnv* environment, const char* callback)
{
    if (environment == nullptr || !environment->ExceptionCheck())
    {
        return;
    }
    environment->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "LUMEN_ANDROID_IME_UI_V1 FAIL callback=%s", callback);
}

jobject AcquireRuntimeActivity(JNIEnv* environment, uint64_t* revision)
{
    if (environment == nullptr || revision == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    *revision = gRuntimeActivityRevision;
    return gRuntimeActivity != nullptr ? environment->NewLocalRef(gRuntimeActivity) : nullptr;
}

bool IsRuntimeActivity(JNIEnv* environment, jobject activity)
{
    if (environment == nullptr || activity == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    return IsRuntimeActivityLocked(environment, activity);
}

bool ReadImeText(JNIEnv* environment, jstring value, jint cursor, std::u16string* text)
{
    if (environment == nullptr || value == nullptr || text == nullptr || cursor < 0 ||
        environment->ExceptionCheck())
    {
        return false;
    }
    const jsize count = environment->GetStringLength(value);
    if (environment->ExceptionCheck() || count > static_cast<jsize>(Libs::ImeCommon::MAX_TEXT_LENGTH) || cursor > count)
    {
        return false;
    }
    const jchar* data = environment->GetStringChars(value, nullptr);
    if (data == nullptr)
    {
        return false;
    }
    static_assert(sizeof(jchar) == sizeof(char16_t));
    bool copied = false;
    try
    {
        text->assign(reinterpret_cast<const char16_t*>(data), static_cast<size_t>(count));
        copied = true;
    } catch (const std::exception&)
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
                            "LUMEN_ANDROID_IME_UI_V1 FAIL callback=text-copy");
    }
    environment->ReleaseStringChars(value, data);
    return copied;
}

} // namespace

namespace Libs::Graphics {

void SyncAndroidImeDialogUi(const ImeCommon::HostSnapshot* snapshot)
{
    // SDL3 owns JNI_OnLoad and supplies an environment for this SDL thread.
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    uint64_t activity_revision = 0;
    jobject activity = AcquireRuntimeActivity(environment, &activity_revision);
    if (activity == nullptr)
    {
        ClearPendingJavaException(environment, "activity-ref");
        return;
    }
    const uint64_t generation = snapshot != nullptr ? snapshot->generation : 0;
    if (generation == gPublishedGeneration && activity_revision == gPublishedActivityRevision)
    {
        environment->DeleteLocalRef(activity);
        return;
    }
    if (snapshot != nullptr &&
        (generation == 0 || snapshot->max_text_length > ImeCommon::MAX_TEXT_LENGTH ||
         snapshot->text.size() > snapshot->max_text_length || snapshot->cursor > snapshot->text.size() ||
         snapshot->title.size() > ImeCommon::MAX_TEXT_LENGTH || snapshot->placeholder.size() > ImeCommon::MAX_TEXT_LENGTH))
    {
        environment->DeleteLocalRef(activity);
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
                            "LUMEN_ANDROID_IME_UI_V1 FAIL callback=snapshot-bounds");
        return;
    }
    jclass activity_class = environment->GetObjectClass(activity);
    jmethodID callback = activity_class != nullptr
        ? environment->GetMethodID(activity_class, "onNativeImeDialog",
                                   "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;IIIII)V")
        : nullptr;
    const auto make_string = [environment](const std::u16string& value) {
        return environment->NewString(reinterpret_cast<const jchar*>(value.data()), static_cast<jsize>(value.size()));
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
        // The core-owner marker occupies bit 63. Java treats its signed long as
        // an opaque token; converting back to uint64_t preserves that marker.
        environment->CallVoidMethod(activity, callback, static_cast<jlong>(generation), text, title, placeholder,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->cursor) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->max_text_length) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->type) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->option) : 0,
                                    snapshot != nullptr ? static_cast<jint>(snapshot->enter_label) : 0);
        if (!environment->ExceptionCheck())
        {
            gPublishedGeneration = generation;
            gPublishedActivityRevision = activity_revision;
        }
    }
    ClearPendingJavaException(environment, "snapshot");
    if (text != nullptr) environment->DeleteLocalRef(text);
    if (title != nullptr) environment->DeleteLocalRef(title);
    if (placeholder != nullptr) environment->DeleteLocalRef(placeholder);
    if (activity_class != nullptr) environment->DeleteLocalRef(activity_class);
    environment->DeleteLocalRef(activity);
}

} // namespace Libs::Graphics

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeAttachImeUi(JNIEnv* environment, jobject activity)
{
    if (environment == nullptr || activity == nullptr)
    {
        return JNI_FALSE;
    }
    jweak replacement = environment->NewWeakGlobalRef(activity);
    if (replacement == nullptr || environment->ExceptionCheck())
    {
        if (replacement != nullptr) environment->DeleteWeakGlobalRef(replacement);
        ClearPendingJavaException(environment, "attach");
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    Libs::Controller::ResetVirtualInputState();
    if (gRuntimeActivity != nullptr) environment->DeleteWeakGlobalRef(gRuntimeActivity);
    gRuntimeActivity = replacement;
    ++gRuntimeActivityRevision;
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeDetachImeUi(JNIEnv* environment, jobject activity)
{
    if (environment == nullptr || activity == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (gRuntimeActivity != nullptr && environment->IsSameObject(gRuntimeActivity, activity))
    {
        Libs::Controller::ResetVirtualInputState();
        environment->DeleteWeakGlobalRef(gRuntimeActivity);
        gRuntimeActivity = nullptr;
        ++gRuntimeActivityRevision;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualButton(
    JNIEnv* environment, jobject activity, jint button, jboolean pressed)
{
    if (button < 0 || static_cast<size_t>(button) >= VIRTUAL_BUTTONS.size()) return;
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (IsRuntimeActivityLocked(environment, activity))
    {
        Libs::Controller::SetVirtualButton(VIRTUAL_BUTTONS[button], pressed == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeVirtualAxis(
    JNIEnv* environment, jobject activity, jint axis, jint value)
{
    if (axis < 0 || static_cast<size_t>(axis) >= VIRTUAL_AXES.size() || value < 0 || value > 255) return;
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (IsRuntimeActivityLocked(environment, activity))
    {
        Libs::Controller::SetVirtualAxis(VIRTUAL_AXES[axis], value);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeResetVirtualController(
    JNIEnv* environment, jobject activity)
{
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (IsRuntimeActivityLocked(environment, activity))
    {
        Libs::Controller::ResetVirtualInputState();
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeGetPresentedFrameCount(
    JNIEnv* environment, jobject activity)
{
    std::lock_guard<std::mutex> lock(gUiMutex);
    return IsRuntimeActivityLocked(environment, activity)
        ? static_cast<jlong>(Libs::Graphics::GetPresentedGuestFrameCount()) : 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeGetRenderTimingSnapshot(
    JNIEnv* environment, jobject activity)
{
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (!IsRuntimeActivityLocked(environment, activity)) return nullptr;
    const auto snapshot = Common::RenderTiming::Read();
    std::string text = "LUMEN_PS5_RENDER_TIMING_V1 enabled=";
    text += snapshot.enabled ? "1" : "0";
    text += " steadyNs=" + std::to_string(snapshot.steady_ns);
    for (std::size_t i = 0; i < Common::RenderTiming::CounterCount; ++i) {
        text += " ";
        text += Common::RenderTiming::CounterNames[i];
        text += "=" + std::to_string(snapshot.values[i]);
    }
    text += " guestPresents=" + std::to_string(Libs::Graphics::GetPresentedGuestFrameCount());
    return environment->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeGetReadbackSamples(
    JNIEnv* environment, jobject activity)
{
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (!IsRuntimeActivityLocked(environment, activity)) return nullptr;
    const auto snapshot = Common::RenderTiming::ReadReadbacks();
    std::string text = "LUMEN_PS5_READBACKS_V1 enabled=";
    text += snapshot.enabled ? "1" : "0";
    text += " completed=" + std::to_string(snapshot.completed);
    text += " count=" + std::to_string(snapshot.count);
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const auto& sample = snapshot.samples[i];
        text += "\nLUMEN_PS5_READBACK_V1 sequence=" + std::to_string(sample.sequence);
        text += " steadyNs=" + std::to_string(sample.steady_ns);
        text += " origin=";
        text += Common::RenderTiming::ReadbackOriginName(sample.origin);
        text += " address=" + std::to_string(sample.request_address);
        text += " size=" + std::to_string(sample.request_size);
        text += " bufferAddress=" + std::to_string(sample.buffer_address);
        text += " bufferSize=" + std::to_string(sample.buffer_size);
        text += " payloadBytes=" + std::to_string(sample.payload_bytes);
        text += " stagingBytes=" + std::to_string(sample.staging_bytes);
        text += " nativeWaitNs=" + std::to_string(sample.native_wait_ns);
    }
    return environment->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeImeDialogUpdate(
    JNIEnv* environment, jobject activity, jlong generation, jstring value, jint cursor)
{
    bool accepted = false;
    try
    {
        std::u16string text;
        accepted = IsRuntimeActivity(environment, activity) && ReadImeText(environment, value, cursor, &text) &&
            Libs::Graphics::SystemImeHostReplaceText(static_cast<uint64_t>(generation), text, static_cast<uint32_t>(cursor));
    } catch (const std::exception&)
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "LUMEN_ANDROID_IME_UI_V1 FAIL callback=update");
    }
    ClearPendingJavaException(environment, "update");
    return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeImeDialogAccept(
    JNIEnv* environment, jobject activity, jlong generation, jstring value, jint cursor)
{
    bool accepted = false;
    try
    {
        // Include the final composing edit before the ordered host Accept.
        std::u16string text;
        accepted = IsRuntimeActivity(environment, activity) && ReadImeText(environment, value, cursor, &text) &&
            Libs::Graphics::SystemImeHostReplaceText(static_cast<uint64_t>(generation), text, static_cast<uint32_t>(cursor)) &&
            Libs::Graphics::SystemImeHostAccept(static_cast<uint64_t>(generation));
    } catch (const std::exception&)
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "LUMEN_ANDROID_IME_UI_V1 FAIL callback=accept");
    }
    ClearPendingJavaException(environment, "accept");
    return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_Ps5RuntimeActivity_nativeImeDialogCancel(JNIEnv* environment, jobject activity, jlong generation)
{
    try
    {
        if (IsRuntimeActivity(environment, activity))
        {
            Libs::Graphics::SystemImeHostCancel(static_cast<uint64_t>(generation));
        }
    } catch (const std::exception&)
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "LUMEN_ANDROID_IME_UI_V1 FAIL callback=cancel");
    }
    ClearPendingJavaException(environment, "cancel");
}
