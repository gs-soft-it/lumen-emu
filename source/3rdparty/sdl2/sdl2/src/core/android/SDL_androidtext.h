/* SPDX-License-Identifier: GPL-2.0-only
 * Original Lumen addition, 2026-10-03: validate Java UTF-16 and encode standard
 * UTF-8 for SDL text events. JNI modified UTF-8 is not an SDL text encoding.
 * The surrounding SDL sources retain their original zlib license notices.
 */
#ifndef LUMEN_SDL_ANDROID_TEXT_H
#define LUMEN_SDL_ANDROID_TEXT_H

#include <stddef.h>
#include <stdint.h>

static int SDL_AndroidUtf16ToUtf8(const uint16_t *text, size_t units,
                                char *output, size_t capacity, size_t *written)
{
    size_t i, count = 0;
    if (written != NULL) *written = 0;
    if (output == NULL || capacity == 0) return 0;
    output[0] = '\0';
    if (text == NULL && units != 0) return 0;
    for (i = 0; i < units; ++i) {
        uint32_t value = text[i];
        size_t bytes;
        /* An embedded NUL cannot be represented by SDL's C-string events. */
        if (value == 0) goto invalid;
        if (value >= 0xd800 && value <= 0xdbff) {
            uint32_t low;
            if (++i >= units) goto invalid;
            low = text[i];
            if (low < 0xdc00 || low > 0xdfff) goto invalid;
            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
        } else if (value >= 0xdc00 && value <= 0xdfff) {
            goto invalid;
        }
        bytes = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
        if (bytes >= capacity - count) goto invalid;
        if (bytes == 1) {
            output[count++] = (char)value;
        } else if (bytes == 2) {
            output[count++] = (char)(0xc0 | (value >> 6));
            output[count++] = (char)(0x80 | (value & 0x3f));
        } else if (bytes == 3) {
            output[count++] = (char)(0xe0 | (value >> 12));
            output[count++] = (char)(0x80 | ((value >> 6) & 0x3f));
            output[count++] = (char)(0x80 | (value & 0x3f));
        } else {
            output[count++] = (char)(0xf0 | (value >> 18));
            output[count++] = (char)(0x80 | ((value >> 12) & 0x3f));
            output[count++] = (char)(0x80 | ((value >> 6) & 0x3f));
            output[count++] = (char)(0x80 | (value & 0x3f));
        }
    }
    output[count] = '\0';
    if (written != NULL) *written = count;
    return 1;
invalid:
    output[0] = '\0';
    return 0;
}
#endif
