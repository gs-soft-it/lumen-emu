// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/presentation/framePacing.h"

#include <cstdio>

int main() {
    using Libs::Graphics::NextVblankWait;
    unsigned failures = 0;
    auto check = [&](bool result, const char* name) {
        if (!result) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
    };
    check(NextVblankWait(0, 16667, 500) == 16167, "ordinary frame pacing");
    check(NextVblankWait(-67, 16667, 500) == 16100, "fractional oversleep correction");
    check(NextVblankWait(67, 16667, 500) == 16234, "fractional undersleep correction");
    check(NextVblankWait(20000, 16667, 500) == 16667, "positive carry bounded to period");
    auto wait = NextVblankWait(-80000, 16667, 150000);
    check(wait == 0, "stall discards accumulated debt");
    wait = NextVblankWait(wait, 16667, 500);
    check(wait == 16167, "first fast frame resumes pacing");
    check(NextVblankWait(-150000, 16667, 500) == 0, "scheduler oversleep bounded");
    check(NextVblankWait(0, 33333, 500) == 32833, "refresh change");
    check(NextVblankWait(0, 16667, 16667) == 0, "exact period");
    check(NextVblankWait(0, 1, 0) == 1, "minimum tick period");
    return failures ? 1 : 0;
}
