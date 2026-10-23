// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Lumen::Gen4 {
struct SessionOptions {
    std::filesystem::path root;
    std::filesystem::path data;
    std::filesystem::path executable;
    std::filesystem::path entry{"eboot.bin"};
    std::vector<std::string> guest_args;
    std::string eboot_digest;
    std::string param_digest;
    std::string event_token;
    unsigned width{1280};
    unsigned height{720};
    bool known_system{};
    bool preflight{};
    bool shader_dump{};
    bool succi_benchmark{};
    std::vector<std::filesystem::path> startup_modules;
};
bool ParseCommand(int argc, char** argv, SessionOptions& options, std::string& error);
bool ValidateContent(SessionOptions& options, std::string& error);
bool ValidateEntry(const SessionOptions& options, const std::filesystem::path& entry,
                   std::string& error);
std::vector<std::string> ManagedArguments(const SessionOptions& options);
void EmitEvent(const SessionOptions& options, const char* state);
int RunSession(const SessionOptions& options, const std::function<void(bool running, int status)>& lifecycle = {});
bool UseKnownSystemModules();
}
