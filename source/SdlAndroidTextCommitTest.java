// SPDX-License-Identifier: GPL-2.0-only
package org.libsdl.app;

public final class SdlAndroidTextCommitTest {
    private static int checks;
    private static void expect(boolean value, String name) {
        ++checks;
        if (!value) throw new AssertionError(name);
    }

    private static final class Editor implements SDLTextInputCommit.Sink {
        final StringBuilder text = new StringBuilder("seed");
        final StringBuilder order = new StringBuilder();
        final boolean multiline, hide;
        boolean finished;
        Editor(boolean multiline, boolean hide) { this.multiline = multiline; this.hide = hide; }
        public void key(char value) {
            if (value == '\n') {
                order.append("R;");
                if (multiline && !finished) text.append('\n');
                else finished = true;
            }
        }
        public void text(String value, int cursor) {
            if (!value.isEmpty()) order.append("T:").append(value).append(';');
            if (!finished) text.append(value);
        }
        public boolean hideOnReturn() {
            if (hide) { order.append("H;"); finished = true; }
            return hide;
        }
    }

    public static void main(String[] args) {
        Editor single = new Editor(false, false);
        expect(!SDLTextInputCommit.dispatch("abc\n", 1, single), "Return delivered instead of hide");
        expect(single.finished && single.text.toString().equals("seedabc"), "prefix committed before single-line accept");
        expect(single.order.toString().equals("T:abc;R;"), "text event before Return and no newline text event");
        Editor multi = new Editor(true, false);
        SDLTextInputCommit.dispatch("abc\r\ndef\nghi", 1, multi);
        expect(multi.text.toString().equals("seedabc\ndef\nghi"), "multiline ordered and CRLF is one Return");
        Editor done = new Editor(false, false);
        SDLTextInputCommit.dispatch("\n", 1, done);
        expect(done.finished && done.text.toString().equals("seed") && done.order.toString().equals("R;"), "lone software Done remains Return");
        Editor hidden = new Editor(false, true);
        expect(SDLTextInputCommit.dispatch("abc\ndef", 1, hidden), "legacy hide hint honored");
        expect(hidden.text.toString().equals("seedabc") && hidden.order.toString().equals("T:abc;H;"), "hide preserves preceding text");
        Editor emoji = new Editor(false, false);
        SDLTextInputCommit.dispatch("\ud83d\ude42", 1, emoji);
        expect(emoji.text.toString().equals("seed\ud83d\ude42"), "valid supplementary text preserved");
        for (String invalid: new String[] {"\ud800", "\udc00", "A\ud800", "A\u0000B", "\n\ud800"}) {
            Editor rejected = new Editor(false, false);
            expect(!SDLTextInputCommit.validText(invalid), "invalid UTF16 rejected");
            SDLTextInputCommit.dispatch(invalid, 1, rejected);
            expect(rejected.order.length() == 0 && rejected.text.toString().equals("seed") && !rejected.finished,
                   "invalid text has no prefix/Return effects");
        }
        expect(SDLTextInputCommit.committedReplacementLength("seedabc", 4, 7, 4, 7) == 0, "preedit does not erase committed seed");
        expect(SDLTextInputCommit.committedReplacementLength("seedabc", -1, -1, 4, 7) == 3, "committed selection replaced");
        expect(SDLTextInputCommit.committedReplacementLength("seedabc", -1, -1, 7, 4) == 3, "reversed selection");
        expect(SDLTextInputCommit.committedReplacementLength("A\ud83d\ude42B", -1, -1, 1, 3) == 1, "selected surrogate pair has one Backspace");
        expect(SDLTextInputCommit.committedReplacementLength("", -1, -1, -1, -1) == 0, "empty fresh connection has no stale replacement");
        expect(SDLTextInputCommit.committedReplacementLength("seed", -1, -1, -1, 3) == 0, "invalid selection cannot invent replacement");
        System.out.println("SDL_ANDROID_TEXT_COMMIT PASS checks=" + checks);
    }
}
