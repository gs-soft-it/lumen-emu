// Lumen PS5 Android embedding, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#include "AndroidPs5VulkanDriver.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "loader/guestExecution.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif

#include <android/api-level.h>
#include <android/log.h>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr const char* LOG_TAG = "LumenAndroidPs5";

bool CanonicalDirectory(const char* text, std::filesystem::path* result) {
    if (text == nullptr || result == nullptr || text[0] != '/') return false;
    std::error_code error;
    const std::filesystem::path requested(text);
    *result = std::filesystem::canonical(requested, error);
    return !error && requested == *result && std::filesystem::is_directory(*result, error)
        && !error;
}

bool RegularFile(const std::filesystem::path& path) {
    const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) return false;
    struct stat status {};
    const bool valid = fstat(file, &status) == 0 && S_ISREG(status.st_mode)
        && status.st_size > 0;
    close(file);
    return valid;
}

bool Dimension(const char* text, std::uint32_t* result) {
    if (text == nullptr || result == nullptr) return false;
    const auto parsed = std::from_chars(text, text + std::strlen(text), *result);
    return parsed.ec == std::errc {} && *parsed.ptr == '\0'
        && *result >= 64 && *result <= 8192;
}

bool Boolean(const char* text) {
    return text != nullptr && (std::strcmp(text, "0") == 0 || std::strcmp(text, "1") == 0);
}

bool RedirectDiagnosticStream(const std::filesystem::path& path, int destination) {
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
    if (file < 0) return false;
    const bool valid = dup2(file, destination) >= 0;
    close(file);
    return valid;
}

int Failure(const char* reason) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
        "LUMEN_ANDROID_PS5_LAUNCH_V1 FAIL native=%s guestExecution=NOT_RUN", reason);
    std::fprintf(stderr,
        "LUMEN_ANDROID_PS5_LAUNCH_V1 FAIL native=%s guestExecution=NOT_RUN\n", reason);
    std::fflush(stderr);
    return 1;
}
} // namespace

extern "C" __attribute__((visibility("default")))
int LumenAndroidPs5MainV1(int argc, char* argv[]) {
    // SDL prepends its executable name. Older diagnostic callers use system
    // mode with seven options; the app adds driver options and the explicit
    // default-OFF frame/audio aggregates and image publication experiment.
    // Older callers remain accepted with every new selector disabled.
    if ((argc != 8 && argc != 10 && argc != 11 && argc != 13) || argv == nullptr)
        return Failure("arguments");
    std::filesystem::path game, data, session;
    Emulator::RunOptions options;
    if (!CanonicalDirectory(argv[1], &game) || !CanonicalDirectory(argv[2], &data)
        || !CanonicalDirectory(argv[3], &session)
        || session.parent_path().parent_path() != data
        || !Dimension(argv[4], &options.config.screen_width)
        || !Dimension(argv[5], &options.config.screen_height)
        || !Boolean(argv[6]) || !Boolean(argv[7])
        || (argc >= 11 && !Boolean(argv[10]))
        || (argc == 13 && (!Boolean(argv[11]) || !Boolean(argv[12]))))
        return Failure("paths-or-options");
#if defined(LUMEN_PS5_DIRECT_ENGINE)
    if ((argc >= 11 && std::strcmp(argv[10], "0") != 0)
        || (argc == 13 && (std::strcmp(argv[11], "0") != 0
                          || std::strcmp(argv[12], "0") != 0)))
        return Failure("legacy-engine-selectors-unavailable");
#else
    if (setenv("LUMEN_PS5_FRAME_PIPELINE_TRACE", argc >= 11 ? argv[10] : "0", 1) != 0)
        return Failure("frame-trace-environment");
    if (setenv("LUMEN_PS5_AUDIO_TIMING_TRACE", argc == 13 ? argv[11] : "0", 1) != 0
        || setenv("LUMEN_PS5_IMAGE_PUBLICATION_SNAPSHOT", argc == 13 ? argv[12] : "0", 1) != 0)
        return Failure("timing-selector-environment");
#endif
    const auto metadata = game / "sce_sys";
    struct stat ps4_metadata {};
    if (!CanonicalDirectory(metadata.c_str(), &options.app0_dir)
        || !RegularFile(metadata / "param.json") || !RegularFile(game / "eboot.bin")
        || lstat((metadata / "param.sfo").c_str(), &ps4_metadata) == 0)
        return Failure("ps5-format");

    if (std::strcmp(argv[6], "1") == 0) {
        std::fflush(stdout);
        std::fflush(stderr);
        if (!RedirectDiagnosticStream(session / "stdout.log", STDOUT_FILENO)
            || !RedirectDiagnosticStream(session / "stderr.log", STDERR_FILENO))
            return Failure("diagnostic-stream");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (android_get_device_api_level() < 35 || (page_size != 4096 && page_size != 16384))
        return Failure("cpu-host");
    if (chdir(data.c_str()) != 0) return Failure("data-directory");
    if (!LumenAndroidPs5ConfigureVulkanDriver(argc >= 10 ? argv[8] : "",
                                             argc >= 10 ? argv[9] : ""))
        return Failure("vulkan-driver-admission");
    options.app0_dir = game;
    // Emulator::Run mounts canonical app0_dir before resolving this guest VFS
    // path, exactly as the CLI. Passing a host path produces no VFS match.
    options.elf = "/app0/eboot.bin";
    options.upstream_import_fallback = std::strcmp(argv[7], "1") == 0;
    options.config.present_mode = Config::PresentMode::Fifo;
    options.config.fullscreen_enabled = true;
#if defined(LUMEN_PS5_DIRECT_ENGINE)
    options.config.splash_enabled = false;
#endif
    if (std::strcmp(argv[6], "1") == 0) {
        options.config.printf_direction = Config::LogDirection::Console;
        options.config.shader_log_direction = Config::LogDirection::File;
        options.config.shader_validation_enabled = true;
        options.config.shader_log_folder = session / "Shaders";
        options.config.command_buffer_dump_folder = session / "Buffers";
    }
    std::fprintf(stderr,
        "LUMEN_ANDROID_PS5_LAUNCH_V1 system=PS5 engine=full backend=%s "
        "ps4Route=denied pageSize=%ld importMode=%s cooperativeStop=unqualified\n",
        Lumen::GuestCpu::ProviderName(), page_size,
        options.upstream_import_fallback ? "upstream-fallback" : "strict");
    Common::VirtualMemory::Init();
    Common::InitializeThreads();
    // This admission belongs to the PS5 DSO/process. Dashboard CPU readiness
    // cannot initialize or publish the complete PS5 embedding's capabilities.
    if (!Loader::GuestExecution::InitializeRuntime()) return Failure("ps5-cpu-admission");
#if defined(LUMEN_PS5_DIRECT_FEX)
    std::fprintf(stderr, "%s\n", Lumen::Ps5::DirectFex::Receipt());
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", Lumen::Ps5::DirectFex::Receipt());
#endif
#if defined(LUMEN_PS5_DIRECT_ENGINE)
    std::fprintf(stderr, "LUMEN_ANDROID_PS5_ENGINE_V1 model=original-direct "
        "renderer=original memory=original services=original legacyPolicies=absent\n");
#endif
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "LUMEN_ANDROID_PS5_CPU_ADMISSION_V1 admitted=1 backend=%s pageSize=%ld",
        Lumen::GuestCpu::ProviderName(), page_size);
    Emulator::Run(options);
    return 0;
}
