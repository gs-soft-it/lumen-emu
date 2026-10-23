// SPDX-License-Identifier: GPL-2.0-only
// Original Lumen addition, 2026-10-03. SDL itself retains its zlib notices.
package org.libsdl.app;

/** Pure event ordering shared by the Android IME bridge and its contract test. */
final class SDLTextInputCommit {
    interface Sink {
        void key(char value);
        void text(String value, int newCursorPosition);
        boolean hideOnReturn();
    }

    static boolean validText(CharSequence text) {
        if (text == null) return false;
        for (int i = 0; i < text.length(); ++i) {
            char value = text.charAt(i);
            if (value == 0 || (value >= 0xdc00 && value <= 0xdfff)) return false;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (++i >= text.length()) return false;
                value = text.charAt(i);
                if (value < 0xdc00 || value > 0xdfff) return false;
            }
        }
        return true;
    }

    static int committedReplacementLength(CharSequence content, int composingStart, int composingEnd,
                                          int selectionStart, int selectionEnd) {
        // Composing text was sent only as SDL_TEXTEDITING, never committed.
        if (content == null || selectionStart < 0 || selectionEnd < 0 ||
            (composingStart >= 0 && composingEnd >= 0)) return 0;
        int begin = Math.max(0, Math.min(selectionStart, selectionEnd));
        int end = Math.min(content.length(), Math.max(selectionStart, selectionEnd));
        int count = 0;
        for (int i = begin; i < end; ++i) {
            char value = content.charAt(i);
            if (value >= 0xd800 && value <= 0xdbff && i + 1 < end &&
                content.charAt(i + 1) >= 0xdc00 && content.charAt(i + 1) <= 0xdfff) ++i;
            ++count;
        }
        return count;
    }

    private static void segment(String text, int begin, int end, int cursor, Sink sink) {
        for (int i = begin; i < end; ++i) sink.key(text.charAt(i));
        sink.text(text.substring(begin, end), cursor);
    }

    static boolean dispatch(CharSequence input, int newCursorPosition, Sink sink) {
        if (!validText(input)) return false;
        String text = input.toString();
        int begin = 0;
        for (int i = 0; i < text.length(); ++i) {
            char value = text.charAt(i);
            if (value != '\n' && value != '\r') continue;
            // Commit the preceding text before Return can finish an editor.
            // Return has exactly one key event channel, including lone Done.
            segment(text, begin, i, newCursorPosition, sink);
            if (sink.hideOnReturn()) return true;
            sink.key('\n');
            if (value == '\r' && i + 1 < text.length() && text.charAt(i + 1) == '\n') ++i;
            begin = i + 1;
        }
        segment(text, begin, text.length(), newCursorPosition, sink);
        return false;
    }

    private SDLTextInputCommit() {}
}
