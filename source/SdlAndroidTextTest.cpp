// SPDX-License-Identifier: GPL-2.0-only
#include "3rdparty/sdl2/sdl2/src/core/android/SDL_androidtext.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
bool Expect(bool value, const char* name)
{
    ++checks;
    if (!value) std::fprintf(stderr, "SDL_ANDROID_TEXT FAIL %s\n", name);
    return value;
}

bool Encodes(const std::vector<uint16_t>& text, const std::string& expected, const char* name)
{
    std::array<char, 128> output {};
    size_t written = SIZE_MAX;
    bool pass = Expect(SDL_AndroidUtf16ToUtf8(text.data(), text.size(), output.data(), output.size(), &written) != 0,
                       name);
    pass &= Expect(written == expected.size() && std::string(output.data()) == expected, name);
    pass &= Expect(SDL_AndroidUtf16ToUtf8(text.data(), text.size(), output.data(), expected.size() + 1, &written) != 0,
                   "exact terminated capacity");
    pass &= Expect(std::string(output.data()) == expected, "exact bytes");
    if (!expected.empty()) {
        output.fill('x'); written = SIZE_MAX;
        pass &= Expect(SDL_AndroidUtf16ToUtf8(text.data(), text.size(), output.data(), expected.size(), &written) == 0 &&
                       output[0] == '\0' && written == 0, "short capacity rejects without visible prefix");
    }
    return pass;
}
} // namespace

int main()
{
    bool pass = true;
    pass &= Encodes({}, "", "empty composition");
    pass &= Encodes({'N', 'o', 'm', 'e'}, "Nome", "ASCII");
    pass &= Encodes({0x00e9, 0x20ac, 0x6f22}, "\xc3\xa9\xe2\x82\xac\xe6\xbc\xa2", "BMP accents and CJK");
    pass &= Encodes({0xd83d, 0xde42}, "\xf0\x9f\x99\x82", "supplementary emoji uses four bytes");
    pass &= Encodes({0xd800, 0xdc00, 0xdbff, 0xdfff}, "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", "plane endpoints");
    pass &= Encodes({'A', 0xd83d, 0xde42, 0x00e9, '\n'}, "A\xf0\x9f\x99\x82\xc3\xa9\n", "mixed commit");
    pass &= Encodes({0x007f, 0x0080, 0x07ff, 0x0800, 0xd7ff, 0xe000},
                    "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80", "encoding boundaries");
    for (const auto& text: std::vector<std::vector<uint16_t>> {
             {0xd800}, {0xdc00}, {0xd800, 0xd800}, {0xdc00, 0xd800}, {'A', 0xd800}, {'A', 0, 'B'}}) {
        std::array<char, 32> output; output.fill('x');
        size_t written = SIZE_MAX;
        pass &= Expect(SDL_AndroidUtf16ToUtf8(text.data(), text.size(), output.data(), output.size(), &written) == 0 &&
                       output[0] == '\0' && written == 0, "malformed UTF16 or embedded NUL rejected");
    }
    char output = 'x';
    pass &= Expect(SDL_AndroidUtf16ToUtf8(nullptr, 1, &output, 1, nullptr) == 0, "null source");
    pass &= Expect(SDL_AndroidUtf16ToUtf8(nullptr, 0, nullptr, 1, nullptr) == 0, "null output");
    pass &= Expect(SDL_AndroidUtf16ToUtf8(nullptr, 0, &output, 0, nullptr) == 0, "zero capacity");
    std::printf("SDL_ANDROID_TEXT %s checks=%u\n", pass ? "PASS" : "FAIL", checks);
    return pass ? 0 : 1;
}
