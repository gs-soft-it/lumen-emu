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

#ifndef _SDL_aaudio_h
#define _SDL_aaudio_h

#include "../SDL_sysaudio.h"
#include <stdbool.h>
#include <aaudio/AAudio.h>

/* Hidden "this" pointer for the audio functions */
#define _THIS   SDL_AudioDevice *this

struct SDL_PrivateAudioData
{
    AAudioStream *stream;

    /* Raw mixing buffer */
    Uint8 *mixbuf;
    int mixlen;
    int frame_size;

    /* Resume device if it was paused automatically */
    int resume;

    /*
     * Lumen modification: AAudio timed writes may complete only part of a
     * buffer. Keep lightweight playback-thread counters so short/zero writes
     * and any tail interrupted by an error or lifecycle cancellation are
     * observable.
     */
    Uint64 write_buffer_calls;
    Uint64 write_api_calls;
    Uint64 write_requested_frames;
    Uint64 write_completed_frames;
    Uint64 write_short_calls;
    Uint64 write_zero_calls;
    Uint64 write_error_calls;
    Uint64 write_incomplete_buffer_calls;
    Uint64 write_incomplete_frames;
    Uint64 write_cancelled_buffer_calls;
    Uint64 write_cancelled_frames;

    /* Lumen modification: opt-in aggregate timing, owned by the write thread.
       No samples or guest addresses are retained by this diagnostic. */
    SDL_bool write_trace_enabled;
    Uint64 write_trace_start_ms;
    Uint64 write_elapsed_ms;
    Uint64 write_duration_ms;
    Uint64 write_max_duration_ms;
    int32_t write_xrun_count;
};
 
void aaudio_ResumeDevices(void);
void aaudio_PauseDevices(void);
SDL_bool aaudio_DetectBrokenPlayState(void);


#endif /* _SDL_aaudio_h */

/* vi: set ts=4 sw=4 expandtab: */
