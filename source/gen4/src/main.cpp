// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/session.h"
#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
    Lumen::Gen4::SessionOptions options;
    std::string error;
    try {
        if (!Lumen::Gen4::ParseCommand(argc, argv, options, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        if (!Lumen::Gen4::ValidateContent(options, error)) {
            Lumen::Gen4::EmitEvent(options, "PREFLIGHT_FAILED");
            std::fprintf(stderr, "%s\n", error.c_str());
            return 3;
        }
        Lumen::Gen4::EmitEvent(options, "PREFLIGHT_OK");
        if (options.preflight) return 0;
        return Lumen::Gen4::RunSession(options);
    } catch (const std::exception& exception) {
        Lumen::Gen4::EmitEvent(options, "SCRIPT_ERROR");
        std::fprintf(stderr, "PS4 runtime failed: %s\n", exception.what());
        return 5;
    }
}
