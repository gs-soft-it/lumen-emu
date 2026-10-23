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

#ifndef SDL_MFIDIGITALQUEUE_H
#define SDL_MFIDIGITALQUEUE_H

#include "SDL_atomic.h"
#include "SDL_stdinc.h"

#define SDL_MFI_DIGITAL_QUEUE_CAPACITY 128

typedef enum SDL_MFIDigitalEventType
{
    SDL_MFI_DIGITAL_EVENT_BUTTON,
    SDL_MFI_DIGITAL_EVENT_HAT
} SDL_MFIDigitalEventType;

typedef struct SDL_MFIDigitalEvent
{
    Uint8 type;
    Uint8 index;
    Uint8 value;
} SDL_MFIDigitalEvent;

typedef struct SDL_MFIDigitalQueue
{
    SDL_SpinLock lock;
    SDL_MFIDigitalEvent events[SDL_MFI_DIGITAL_QUEUE_CAPACITY];
    Uint16 head;
    Uint16 count;
    SDL_bool accepting;
    SDL_bool overflowed;
} SDL_MFIDigitalQueue;

/* Reset is only valid while no producer can access the queue. */
static SDL_INLINE void
SDL_MFIDigitalQueueReset(SDL_MFIDigitalQueue *queue)
{
    SDL_memset(queue, 0, sizeof(*queue));
    queue->accepting = SDL_TRUE;
}

static SDL_INLINE SDL_bool
SDL_MFIDigitalQueueEnqueue(SDL_MFIDigitalQueue *queue, Uint8 type, Uint8 index, Uint8 value)
{
    Uint16 tail;

    SDL_AtomicLock(&queue->lock);
    if (!queue->accepting) {
        SDL_AtomicUnlock(&queue->lock);
        return SDL_FALSE;
    }
    if (queue->overflowed) {
        SDL_AtomicUnlock(&queue->lock);
        return SDL_FALSE;
    }
    if (queue->count == SDL_MFI_DIGITAL_QUEUE_CAPACITY) {
        queue->overflowed = SDL_TRUE;
        queue->head = 0;
        queue->count = 0;
        SDL_AtomicUnlock(&queue->lock);
        return SDL_FALSE;
    }

    tail = (Uint16)((queue->head + queue->count) % SDL_MFI_DIGITAL_QUEUE_CAPACITY);
    queue->events[tail].type = type;
    queue->events[tail].index = index;
    queue->events[tail].value = value;
    ++queue->count;
    SDL_AtomicUnlock(&queue->lock);
    return SDL_TRUE;
}

static SDL_INLINE int
SDL_MFIDigitalQueueDrain(SDL_MFIDigitalQueue *queue, SDL_MFIDigitalEvent *events, int max_events)
{
    int count;
    int i;

    if (max_events <= 0) {
        return 0;
    }

    SDL_AtomicLock(&queue->lock);
    if (queue->overflowed) {
        SDL_AtomicUnlock(&queue->lock);
        return 0;
    }
    count = SDL_min((int)queue->count, max_events);
    for (i = 0; i < count; ++i) {
        events[i] = queue->events[(queue->head + i) % SDL_MFI_DIGITAL_QUEUE_CAPACITY];
    }
    queue->head = (Uint16)((queue->head + count) % SDL_MFI_DIGITAL_QUEUE_CAPACITY);
    queue->count = (Uint16)(queue->count - count);
    SDL_AtomicUnlock(&queue->lock);
    return count;
}

static SDL_INLINE SDL_bool
SDL_MFIDigitalQueueTakeOverflow(SDL_MFIDigitalQueue *queue)
{
    SDL_bool overflowed;

    SDL_AtomicLock(&queue->lock);
    overflowed = queue->overflowed;
    queue->overflowed = SDL_FALSE;
    SDL_AtomicUnlock(&queue->lock);
    return overflowed;
}

static SDL_INLINE void
SDL_MFIDigitalQueueStop(SDL_MFIDigitalQueue *queue)
{
    SDL_AtomicLock(&queue->lock);
    queue->accepting = SDL_FALSE;
    queue->head = 0;
    queue->count = 0;
    queue->overflowed = SDL_FALSE;
    SDL_AtomicUnlock(&queue->lock);
}

#endif /* SDL_MFIDIGITALQUEUE_H */
