/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_AAUDIO

#include "SDL_audio.h"
#include "SDL_loadso.h"
#include "SDL_timer.h"
#include "../SDL_audio_c.h"
#include "../../core/android/SDL_android.h"
#include "SDL_aaudio.h"
#include <pthread.h>
#include <stdlib.h>

/* Debug */
#if 0
#  define LOGI(...) SDL_Log(__VA_ARGS__);
#else
#  define LOGI(...)
#endif

typedef struct AAUDIO_Data
{
    AAudioStreamBuilder *builder;
    void *handle;
#define SDL_PROC(ret,func,params) ret (*func) params;
#  include "SDL_aaudiofuncs.h"
#undef SDL_PROC
} AAUDIO_Data;
static AAUDIO_Data ctx;

/*
 * Lumen modification: Android's event thread calls the helpers at the end of
 * this file while SDL can open or close the audio device on another thread.
 * Serialize publication and lifetime of the global device pointers so those
 * helpers never observe a partial or freed SDL_PrivateAudioData/AAudioStream.
 */
static pthread_mutex_t aaudioDeviceLock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDevice *audioDevice = NULL;
static SDL_AudioDevice *captureDevice = NULL;

static void
aaudio_DeviceStateMutexFailure(const char *operation, int errorCode)
{
    SDL_SetError("AAudio device state mutex %s failed: %d", operation, errorCode);
    abort();
}

static void
aaudio_LockDeviceState(void)
{
    const int result = pthread_mutex_lock(&aaudioDeviceLock);
    if (result != 0) {
        aaudio_DeviceStateMutexFailure("lock", result);
    }
}

static void
aaudio_UnlockDeviceState(void)
{
    const int result = pthread_mutex_unlock(&aaudioDeviceLock);
    if (result != 0) {
        aaudio_DeviceStateMutexFailure("unlock", result);
    }
}

static int aaudio_LoadFunctions(AAUDIO_Data *data)
{
#define SDL_PROC(ret,func,params)                                                                   \
    do {                                                                                            \
        data->func = SDL_LoadFunction(data->handle, #func);                                         \
        if (! data->func) {                                                                         \
            return SDL_SetError("Couldn't load AAUDIO function %s: %s", #func, SDL_GetError());     \
        }                                                                                           \
    } while (0);
#include "SDL_aaudiofuncs.h"
#undef SDL_PROC
    return 0;
}

void aaudio_errorCallback( AAudioStream *stream, void *userData, aaudio_result_t error );
void aaudio_errorCallback( AAudioStream *stream, void *userData, aaudio_result_t error )
{
    LOGI( "SDL aaudio_errorCallback: %d - %s", error, ctx.AAudio_convertResultToText( error ) );
}

#define LIB_AAUDIO_SO "libaaudio.so"

/* Lumen modification: only scalar aggregates, after a complete write attempt.
   The live stream is never queried here, so close can log its owned snapshot
   after releasing the lifetime lock. */
static void
aaudio_LogWriteStats(const char *event, const struct SDL_PrivateAudioData *stats)
{
    const SDL_bool framesAccounted =
        stats->write_requested_frames == stats->write_completed_frames +
        stats->write_incomplete_frames + stats->write_cancelled_frames ? SDL_TRUE : SDL_FALSE;
    SDL_Log("LUMEN_AAUDIO_WRITE_V1 event=%s buffers=%" SDL_PRIu64
            " calls=%" SDL_PRIu64 " requested=%" SDL_PRIu64
            " completed=%" SDL_PRIu64 " short=%" SDL_PRIu64
            " zero=%" SDL_PRIu64 " errors=%" SDL_PRIu64
            " incomplete_buffers=%" SDL_PRIu64
            " incomplete_frames=%" SDL_PRIu64
            " cancelled_buffers=%" SDL_PRIu64
            " cancelled_frames=%" SDL_PRIu64
            " trace=%u elapsed_ms=%" SDL_PRIu64
            " write_ms=%" SDL_PRIu64 " max_write_ms=%" SDL_PRIu64
            " xruns=%d frames_accounted=%u",
            event, stats->write_buffer_calls, stats->write_api_calls,
            stats->write_requested_frames, stats->write_completed_frames,
            stats->write_short_calls, stats->write_zero_calls, stats->write_error_calls,
            stats->write_incomplete_buffer_calls, stats->write_incomplete_frames,
            stats->write_cancelled_buffer_calls, stats->write_cancelled_frames,
            (unsigned int)stats->write_trace_enabled, stats->write_elapsed_ms,
            stats->write_duration_ms, stats->write_max_duration_ms,
            (int)stats->write_xrun_count, (unsigned int)framesAccounted);
}

static int
aaudio_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *private = NULL;
    SDL_bool iscapture = this->iscapture;
    SDL_bool streamStarted = SDL_FALSE;
    aaudio_result_t res;
    LOGI(__func__);

    if (iscapture) {
        if (!Android_JNI_RequestPermission("android.permission.RECORD_AUDIO")) {
            LOGI("This app doesn't have RECORD_AUDIO permission");
            return SDL_SetError("This app doesn't have RECORD_AUDIO permission");
        }
    }

    aaudio_LockDeviceState();

    SDL_assert((captureDevice == NULL) || !iscapture);
    SDL_assert((audioDevice == NULL) || iscapture);

    this->hidden = (struct SDL_PrivateAudioData *) SDL_calloc(1, (sizeof *this->hidden));
    if (this->hidden == NULL) {
        SDL_OutOfMemory();
        goto failure;
    }
    private = this->hidden;
    {
        const char *trace = SDL_getenv("LUMEN_AUDIO_OUTPUT_TRACE");
        private->write_trace_enabled = trace != NULL && SDL_strcmp(trace, "1") == 0 ? SDL_TRUE : SDL_FALSE;
    }

    ctx.AAudioStreamBuilder_setSampleRate(ctx.builder, this->spec.freq);
    ctx.AAudioStreamBuilder_setChannelCount(ctx.builder, this->spec.channels);
    {
        aaudio_direction_t direction = (iscapture ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
        ctx.AAudioStreamBuilder_setDirection(ctx.builder, direction);
    }
    {
        aaudio_format_t format = AAUDIO_FORMAT_PCM_FLOAT;
        if (this->spec.format == AUDIO_S16SYS) {
            format = AAUDIO_FORMAT_PCM_I16;
        } else if (this->spec.format == AUDIO_S16SYS) {
            format = AAUDIO_FORMAT_PCM_FLOAT;
        }
        ctx.AAudioStreamBuilder_setFormat(ctx.builder, format);
    }

    ctx.AAudioStreamBuilder_setErrorCallback( ctx.builder, aaudio_errorCallback, private );

    LOGI("AAudio Try to open %u hz %u bit chan %u %s samples %u",
          this->spec.freq, SDL_AUDIO_BITSIZE(this->spec.format),
          this->spec.channels, (this->spec.format & 0x1000) ? "BE" : "LE", this->spec.samples);

    res = ctx.AAudioStreamBuilder_openStream(ctx.builder, &private->stream);
    if (res != AAUDIO_OK) {
        LOGI("SDL Failed AAudioStreamBuilder_openStream %d", res);
        SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        goto failure;
    }

    this->spec.freq = ctx.AAudioStream_getSampleRate(private->stream);
    this->spec.channels = ctx.AAudioStream_getChannelCount(private->stream);
    {
        aaudio_format_t fmt = ctx.AAudioStream_getFormat(private->stream);
        if (fmt == AAUDIO_FORMAT_PCM_I16) {
            this->spec.format = AUDIO_S16SYS;
        } else if (fmt == AAUDIO_FORMAT_PCM_FLOAT) {
            this->spec.format = AUDIO_F32SYS;
        }
    }

    LOGI("AAudio Try to open %u hz %u bit chan %u %s samples %u",
          this->spec.freq, SDL_AUDIO_BITSIZE(this->spec.format),
          this->spec.channels, (this->spec.format & 0x1000) ? "BE" : "LE", this->spec.samples);

    SDL_CalculateAudioSpec(&this->spec);

    /* Allocate mixing buffer */
    if (!iscapture) {
        private->mixlen = this->spec.size;
        private->mixbuf = (Uint8 *) SDL_malloc(private->mixlen);
        if (private->mixbuf == NULL) {
            SDL_OutOfMemory();
            goto failure;
        }
        SDL_memset(private->mixbuf, this->spec.silence, this->spec.size);
    }

    private->frame_size = this->spec.channels * (SDL_AUDIO_BITSIZE(this->spec.format) / 8);

    res = ctx.AAudioStream_requestStart(private->stream);
    if (res != AAUDIO_OK) {
        LOGI("SDL Failed AAudioStream_requestStart %d iscapture:%d", res, iscapture);
        SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        goto failure;
    }
    streamStarted = SDL_TRUE;

    /* Publish only a complete, started device to the Android event thread. */
    if (iscapture) {
        captureDevice = this;
    } else {
        audioDevice = this;
    }

    LOGI("SDL AAudioStream_requestStart OK");
    aaudio_UnlockDeviceState();
    if (!iscapture && private->write_trace_enabled) {
        SDL_Log("LUMEN_AAUDIO_DEVICE_V1 rate=%d channels=%u format=%u callback_frames=%u buffer_bytes=%u frame_bytes=%d",
                this->spec.freq, (unsigned int)this->spec.channels, (unsigned int)this->spec.format,
                (unsigned int)this->spec.samples, (unsigned int)this->spec.size, private->frame_size);
    }
    return 0;

failure:
    if (private != NULL) {
        if (private->stream != NULL) {
            if (streamStarted) {
                (void)ctx.AAudioStream_requestStop(private->stream);
            }
            (void)ctx.AAudioStream_close(private->stream);
            private->stream = NULL;
        }
        SDL_free(private->mixbuf);
        SDL_free(private);
        this->hidden = NULL;
    }
    aaudio_UnlockDeviceState();
    return -1;
}

static void
aaudio_CloseDevice(_THIS)
{
    struct SDL_PrivateAudioData *private;
    struct SDL_PrivateAudioData statsSnapshot;
    SDL_bool haveWriteStats = SDL_FALSE;
    aaudio_result_t res;
    LOGI(__func__);

    SDL_zero(statsSnapshot);

    aaudio_LockDeviceState();
    private = this->hidden;

    /* Unpublish first; helpers already in flight hold aaudioDeviceLock. */
    if (this->iscapture) {
        SDL_assert((captureDevice == this) || (captureDevice == NULL));
        if (captureDevice == this) {
            captureDevice = NULL;
        }
    } else {
        SDL_assert((audioDevice == this) || (audioDevice == NULL));
        if (audioDevice == this) {
            audioDevice = NULL;
        }
    }

    if (private != NULL && private->stream != NULL) {
        res = ctx.AAudioStream_requestStop(private->stream);
        if (res != AAUDIO_OK) {
            LOGI("SDL Failed AAudioStream_requestStop %d", res);
            SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        }

        res = ctx.AAudioStream_close(private->stream);
        if (res != AAUDIO_OK) {
            LOGI("SDL Failed AAudioStreamBuilder_delete %d", res);
            SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        }
        private->stream = NULL;
    }

    if (private != NULL) {
        if (!this->iscapture && private->write_buffer_calls != 0) {
            statsSnapshot = *private;
            haveWriteStats = SDL_TRUE;
        }
        SDL_free(private->mixbuf);
        SDL_free(private);
        this->hidden = NULL;
    }
    aaudio_UnlockDeviceState();

    /* Logging callbacks must not run under aaudioDeviceLock. */
    if (haveWriteStats) {
        aaudio_LogWriteStats("close", &statsSnapshot);
    }
}

static Uint8 *
aaudio_GetDeviceBuf(_THIS)
{
    struct SDL_PrivateAudioData *private = this->hidden;
    return private->mixbuf;
}

static void
aaudio_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *private = this->hidden;
    const int requestedFrames = private->mixlen / private->frame_size;
    int completedFrames = 0;
    SDL_bool cancelled = SDL_FALSE;
    SDL_bool writeFailed = SDL_FALSE;
    const Uint64 writeStart = SDL_GetTicks64();
    const Uint64 writeDeadline = writeStart + 1000;

    /*
     * Lumen modification: AAudioStream_write() may return zero or a partial
     * frame count when its timeout expires. SDL replaces mixbuf after this
     * function returns, so advance through the unwritten tail instead of
     * silently discarding it. Each individual write remains bounded so pause
     * and shutdown requests can cancel the loop without a lifetime lock.
     */
    const int64_t timeoutNanoseconds = 1 * 1000 * 1000; /* 1 ms per attempt */

    /* SDL joins this audio thread before aaudio_CloseDevice frees hidden. Do
       not hold aaudioDeviceLock here: requestPause must remain able to release
       a vendor write that ignores its nominal timeout. */
    private->write_buffer_calls++;
    private->write_requested_frames += (Uint64)requestedFrames;
    if (private->write_trace_enabled && private->write_buffer_calls == 1) {
        private->write_trace_start_ms = writeStart;
    }
    while (completedFrames < requestedFrames) {
        const int remainingFrames = requestedFrames - completedFrames;
        const Uint8 *buffer = private->mixbuf + (completedFrames * private->frame_size);
        const aaudio_result_t res = ctx.AAudioStream_write(
            private->stream, buffer, remainingFrames, timeoutNanoseconds);

        private->write_api_calls++;
        if (res < 0) {
            writeFailed = SDL_TRUE;
            break;
        }
        if (res > remainingFrames) {
            writeFailed = SDL_TRUE;
            break;
        }
        if (res == 0) {
            private->write_zero_calls++;
        } else {
            completedFrames += (int)res;
            private->write_completed_frames += (Uint64)res;
            if (res < remainingFrames) {
                private->write_short_calls++;
            }
        }

        if (SDL_AtomicGet(&this->shutdown) ||
            !SDL_AtomicGet(&this->enabled) ||
            SDL_AtomicGet(&this->paused)) {
            cancelled = SDL_TRUE;
            break;
        }
        /* One second for a single SDL buffer is broken-stream recovery, not
           ordinary timed-write backpressure. Use a monotonic deadline because
           a timeout is a maximum wait, not a minimum. */
        if (SDL_GetTicks64() >= writeDeadline) {
            writeFailed = SDL_TRUE;
            break;
        }
    }

    if (completedFrames < requestedFrames) {
        const Uint64 incompleteFrames = (Uint64)(requestedFrames - completedFrames);
        if (SDL_AtomicGet(&this->shutdown) ||
            !SDL_AtomicGet(&this->enabled) ||
            SDL_AtomicGet(&this->paused)) {
            cancelled = SDL_TRUE;
        }
        if (writeFailed && !cancelled) {
            private->write_error_calls++;
        }
        if (cancelled) {
            private->write_cancelled_buffer_calls++;
            private->write_cancelled_frames += incompleteFrames;
        } else {
            private->write_incomplete_buffer_calls++;
            private->write_incomplete_frames += incompleteFrames;
        }
    }

    if (private->write_trace_enabled) {
        const Uint64 writeEnd = SDL_GetTicks64();
        const Uint64 duration = writeEnd - writeStart;
        private->write_elapsed_ms = writeEnd - private->write_trace_start_ms;
        private->write_duration_ms += duration;
        if (duration > private->write_max_duration_ms) {
            private->write_max_duration_ms = duration;
        }
        if (private->write_buffer_calls <= 3 ||
            (private->write_buffer_calls & (private->write_buffer_calls - 1)) == 0) {
            /* Already-loaded getter, only on the thread that owns writes.
               Diagnostics never alter the retry deadline or PCM tail. */
            private->write_xrun_count = ctx.AAudioStream_getXRunCount(private->stream);
            aaudio_LogWriteStats("checkpoint", private);
        }
    }
}

static int
aaudio_CaptureFromDevice(_THIS, void *buffer, int buflen)
{
    struct SDL_PrivateAudioData *private = this->hidden;
    aaudio_result_t res;
    int64_t timeoutNanoseconds = 8 * 1000 * 1000; /* 8 ms */
    res = ctx.AAudioStream_read(private->stream,  buffer, buflen / private->frame_size, timeoutNanoseconds);
    if (res < 0) {
        LOGI("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
        return -1;
    }
    LOGI("SDL AAudio capture:%d frames, wanted:%d frames", (int)res, buflen / private->frame_size);
    return res * private->frame_size;
}

static void
aaudio_Deinitialize(void)
{
    LOGI(__func__);
    aaudio_LockDeviceState();
    audioDevice = NULL;
    captureDevice = NULL;
    if (ctx.handle) {
        if (ctx.builder) {
            aaudio_result_t res;
            res = ctx.AAudioStreamBuilder_delete(ctx.builder);
            if (res != AAUDIO_OK) {
                SDL_SetError("Failed AAudioStreamBuilder_delete %s", ctx.AAudio_convertResultToText(res));
            }
        }
        SDL_UnloadObject(ctx.handle);
    }
    ctx.handle = NULL;
    ctx.builder = NULL;
    aaudio_UnlockDeviceState();
    LOGI("End AAUDIO %s", SDL_GetError());
}

static SDL_bool
aaudio_Init(SDL_AudioDriverImpl *impl)
{
    aaudio_result_t res;
    LOGI(__func__);

    /* AAudio was introduced in Android 8.0, but has reference counting crash issues in that release,
     * so don't use it until 8.1.
     *
     * See https://github.com/google/oboe/issues/40 for more information.
     */
    if (SDL_GetAndroidSDKVersion() < 27) {
        return SDL_FALSE;
    }

    SDL_zero(ctx);

    ctx.handle = SDL_LoadObject(LIB_AAUDIO_SO);
    if (ctx.handle == NULL) {
        LOGI("SDL couldn't find " LIB_AAUDIO_SO);
        goto failure;
    }

    if (aaudio_LoadFunctions(&ctx) < 0) {
        goto failure;
    }

    res = ctx.AAudio_createStreamBuilder(&ctx.builder);
    if (res != AAUDIO_OK) {
        LOGI("SDL Failed AAudio_createStreamBuilder %d", res);
        goto failure;
    }

    if (ctx.builder == NULL) {
        LOGI("SDL Failed AAudio_createStreamBuilder - builder NULL");
        goto failure;
    }

    impl->Deinitialize = aaudio_Deinitialize;
    impl->OpenDevice = aaudio_OpenDevice;
    impl->CloseDevice = aaudio_CloseDevice;
    impl->PlayDevice = aaudio_PlayDevice;
    impl->GetDeviceBuf = aaudio_GetDeviceBuf;
    impl->CaptureFromDevice = aaudio_CaptureFromDevice;

    /* and the capabilities */
    impl->HasCaptureSupport = SDL_TRUE;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->OnlyHasDefaultCaptureDevice = SDL_TRUE;
    impl->CloseDeviceBeforeMixerLockDestroy = SDL_TRUE;

    /* this audio target is available. */
    LOGI("SDL aaudio_Init OK");
    return SDL_TRUE;

failure:
    if (ctx.handle) {
        if (ctx.builder) {
            ctx.AAudioStreamBuilder_delete(ctx.builder);
        }
        SDL_UnloadObject(ctx.handle);
    }
    ctx.handle = NULL;
    ctx.builder = NULL;
    return SDL_FALSE;
}

AudioBootStrap aaudio_bootstrap = {
    "AAudio", "AAudio audio driver", aaudio_Init, SDL_FALSE
};

/* Pause (block) all non already paused audio devices by taking their mixer lock */
void aaudio_PauseDevices(void)
{
    /* TODO: Handle multiple devices? */
    struct SDL_PrivateAudioData *private;
    aaudio_LockDeviceState();
    if (audioDevice != NULL && audioDevice->hidden != NULL &&
        !SDL_AtomicGet(&audioDevice->shutdown)) {
        private = (struct SDL_PrivateAudioData *) audioDevice->hidden;

        if (private->stream) {
            aaudio_result_t res = ctx.AAudioStream_requestPause(private->stream);
            if (res != AAUDIO_OK) {
                LOGI("SDL Failed AAudioStream_requestPause %d", res);
                SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
            }
        }

        if (SDL_AtomicGet(&audioDevice->paused)) {
            /* The device is already paused, leave it alone */
            private->resume = SDL_FALSE;
        } else {
            SDL_LockMutex(audioDevice->mixer_lock);
            if (SDL_AtomicGet(&audioDevice->shutdown)) {
                SDL_UnlockMutex(audioDevice->mixer_lock);
                private->resume = SDL_FALSE;
            } else {
                SDL_AtomicSet(&audioDevice->paused, 1);
                private->resume = SDL_TRUE;
            }
        }
    }

    if (captureDevice != NULL && captureDevice->hidden != NULL &&
        !SDL_AtomicGet(&captureDevice->shutdown)) {
        private = (struct SDL_PrivateAudioData *) captureDevice->hidden;

        if (private->stream) {
            /* Pause() isn't implemented for 'capture', use Stop() */
            aaudio_result_t res = ctx.AAudioStream_requestStop(private->stream);
            if (res != AAUDIO_OK) {
                LOGI("SDL Failed AAudioStream_requestStop %d", res);
                SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
            }
        }

        if (SDL_AtomicGet(&captureDevice->paused)) {
            /* The device is already paused, leave it alone */
            private->resume = SDL_FALSE;
        } else {
            SDL_LockMutex(captureDevice->mixer_lock);
            if (SDL_AtomicGet(&captureDevice->shutdown)) {
                SDL_UnlockMutex(captureDevice->mixer_lock);
                private->resume = SDL_FALSE;
            } else {
                SDL_AtomicSet(&captureDevice->paused, 1);
                private->resume = SDL_TRUE;
            }
        }
    }
    aaudio_UnlockDeviceState();
}

/* Resume (unblock) all non already paused audio devices by releasing their mixer lock */
void aaudio_ResumeDevices(void)
{
    /* TODO: Handle multiple devices? */
    struct SDL_PrivateAudioData *private;
    aaudio_LockDeviceState();
    if (audioDevice != NULL && audioDevice->hidden != NULL &&
        !SDL_AtomicGet(&audioDevice->shutdown)) {
        private = (struct SDL_PrivateAudioData *) audioDevice->hidden;

        if (private->resume) {
            SDL_AtomicSet(&audioDevice->paused, 0);
            private->resume = SDL_FALSE;
            SDL_UnlockMutex(audioDevice->mixer_lock);
        }

        if (private->stream) {
            aaudio_result_t res = ctx.AAudioStream_requestStart(private->stream);
            if (res != AAUDIO_OK) {
                LOGI("SDL Failed AAudioStream_requestStart %d", res);
                SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
            }
        }
    }

    if (captureDevice != NULL && captureDevice->hidden != NULL &&
        !SDL_AtomicGet(&captureDevice->shutdown)) {
        private = (struct SDL_PrivateAudioData *) captureDevice->hidden;

        if (private->resume) {
            SDL_AtomicSet(&captureDevice->paused, 0);
            private->resume = SDL_FALSE;
            SDL_UnlockMutex(captureDevice->mixer_lock);
        }

        if (private->stream) {
            aaudio_result_t res = ctx.AAudioStream_requestStart(private->stream);
            if (res != AAUDIO_OK) {
                LOGI("SDL Failed AAudioStream_requestStart %d", res);
                SDL_SetError("%s : %s", __func__, ctx.AAudio_convertResultToText(res));
            }
        }
    }
    aaudio_UnlockDeviceState();
}

/* Legacy SDL used getTimestamp() here to diagnose a write that stopped
   progressing. That query is not safe from this event-thread call site. */
SDL_bool aaudio_DetectBrokenPlayState( void )
{
    /* Android requires AAudioStream_getTimestamp() to run on the same thread
       as stream writes. SDL calls this hook from the video/event thread, so
       using the legacy detector races AAudioStream_write() and can crash in
       libaaudio. Keep the timed write and lifecycle serialization instead. */
    return SDL_FALSE;
}

#endif /* SDL_AUDIO_DRIVER_AAUDIO */

/* vi: set ts=4 sw=4 expandtab: */
