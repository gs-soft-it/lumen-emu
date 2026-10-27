// SPDX-License-Identifier: GPL-2.0-only
// Compile the actual CLI parser in its original source directory. In particular,
// do not include emulator.h first: that would hide a wrong quoted-header lookup.
#define main LumenDirectCliUnusedEntryPoint
#include "../compatibility/src/main.cpp"
#undef main

#include "DirectCliForwardingBoundary.h"

#include <chrono>
#include <fstream>

static bool CheckForwarding(const std::filesystem::path& root, bool customized) {
    std::vector<std::string> arguments {"direct-cli-fixture", "--game", root.string()};
    if (customized) {
        const std::vector<std::string> overrides {
            "--cpu-translator", "fex", "--screen-width", "960", "--screen-height", "540",
            "--user-name", "CLI boundary", "--user-id", "1234", "--present-mode", "Fifo",
            "--console-language", "4", "--tessellation", "--upstream-import-fallback",
            "--game-patch", (std::filesystem::canonical(root).parent_path() / "patch.json").string(),
            "--shader-validation", "true",
            "--readback-linear-images", "true", "--shader-optimization-type", "Performance",
            "--printf-direction", "File", "--printf-output-file", "run.log",
            "--keymap", "Cross=Space",
        };
        arguments.insert(arguments.end(), overrides.begin(), overrides.end());
    }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    Emulator::RunOptions options;
    bool show_help = false;
    if (!ParseArgs(static_cast<int>(argv.size()), argv.data(), options, show_help) || show_help) {
        std::fprintf(stderr, "DIRECT_CLI_FORWARDING FAIL: production parser rejected fixture\n");
        return false;
    }
    const auto offset = [&](const auto& member) {
        return static_cast<std::size_t>(reinterpret_cast<const char*>(&member) -
                                        reinterpret_cast<const char*>(&options));
    };
    const DirectCliOptionsLayout caller_layout {
        sizeof(Emulator::RunOptions), sizeof(Config::ConfigOptions),
        offset(options.app0_dir), offset(options.elf), offset(options.game_patch),
        offset(options.upstream_import_fallback), offset(options.config.tessellation_enabled),
        offset(options.config.printf_direction),
    };
    const auto canonical_root = std::filesystem::canonical(root).string();
    return CheckDirectCliOptionsAtEngineBoundary(options, caller_layout,
                                                 canonical_root.c_str(), customized);
}

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto work = std::filesystem::temp_directory_path() /
        ("lumen-direct-cli-forwarding-" + std::to_string(nonce));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup {work};
    const auto root = work / "extracted root";
    std::filesystem::create_directories(root);
    std::ofstream(root / "eboot.bin", std::ios::binary) << "not guest code";
    std::ofstream(work / "patch.json") << "{}";
    if (!CheckForwarding(root / ".", false) || !CheckForwarding(root / ".", true)) return 1;
    std::printf("DIRECT_CLI_FORWARDING PASS: canonical root and config reached engine ABI; "
                "no guest execution\n");
    return 0;
}
