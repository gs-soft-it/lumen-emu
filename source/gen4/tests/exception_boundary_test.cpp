// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include <stdexcept>
#include <string_view>

int main() {
    // The CPU facade is force-linked by lumen_gen4_guest_cpu. Its exception
    // boundary must not replace libc++'s standard exception type identities.
    try {
        throw std::runtime_error("runtime boundary");
    } catch (const std::exception& error) {
        if (std::string_view{error.what()} != "runtime boundary") return 1;
    } catch (...) {
        return 2;
    }
    try {
        throw std::length_error("length boundary");
    } catch (const std::exception& error) {
        if (std::string_view{error.what()} != "length boundary") return 3;
    } catch (...) {
        return 4;
    }
    std::puts("Gen4 shared CPU exception identity PASS");
}
