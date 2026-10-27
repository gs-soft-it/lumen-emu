// SPDX-License-Identifier: GPL-2.0-only
#include <emulator.h>

#include "DirectCliForwardingBoundary.h"

#include <cstdio>

bool CheckDirectCliOptionsAtEngineBoundary(const Emulator::RunOptions& options,
                                          const DirectCliOptionsLayout& caller_layout,
                                          const char* expected_root, bool customized) {
    const auto offset = [&](const auto& member) {
        return static_cast<std::size_t>(reinterpret_cast<const char*>(&member) -
                                        reinterpret_cast<const char*>(&options));
    };
    const DirectCliOptionsLayout consumer_layout {
        sizeof(Emulator::RunOptions), sizeof(Config::ConfigOptions),
        offset(options.app0_dir), offset(options.elf), offset(options.game_patch),
        offset(options.upstream_import_fallback), offset(options.config.tessellation_enabled),
        offset(options.config.printf_direction),
    };
    if (caller_layout != consumer_layout) {
        std::fprintf(stderr, "DIRECT_CLI_FORWARDING FAIL: CLI/engine options ABI differs\n");
        return false;
    }
    const auto& config = options.config;
    const bool common = options.app0_dir == std::filesystem::path(expected_root) &&
        options.elf == std::filesystem::path("/app0/eboot.bin") && options.addcont_dir.empty() &&
        config.guest_arguments == std::vector<std::string>{""} && config.splash_enabled &&
        !config.network_connected && !config.hdr_enabled && !config.hide_cursor_enabled &&
        !config.skip_notice_screen;
    const bool configured = customized
        ? options.game_patch == std::filesystem::path(expected_root).parent_path() / "patch.json" &&
          options.upstream_import_fallback && config.tessellation_enabled &&
          config.screen_width == 960 && config.screen_height == 540 &&
          config.user_name == "CLI boundary" && config.user_id == 1234 &&
          config.present_mode == Config::PresentMode::Fifo && config.console_language == 4 &&
          config.shader_validation_enabled && config.readback_linear_images &&
          config.shader_optimization_type == Config::ShaderOptimizationType::Performance &&
          config.printf_direction == Config::LogDirection::File &&
          config.printf_output_file == std::filesystem::path("run.log") &&
          config.keymap == Config::Keymap{"Cross=Space"}
        : options.game_patch.empty() && !options.upstream_import_fallback &&
          !config.tessellation_enabled && config.screen_width == 1280 &&
          config.screen_height == 720 && config.user_name == "Kyty" &&
          config.user_id == Config::DEFAULT_USER_ID &&
          config.present_mode == Config::PresentMode::Mailbox &&
          config.printf_direction == Config::LogDirection::Silent && config.keymap.empty();
    if (!common || !configured) {
        std::fprintf(stderr, "DIRECT_CLI_FORWARDING FAIL: engine received incorrect %s options\n",
                     customized ? "customized" : "default");
        return false;
    }
    return true;
}
