// SPDX-License-Identifier: GPL-2.0-only
// Lumen, 2026-10-02: execute the complete engine's existing CPU/HLE contracts
// on Android without a Java surface, ROM, controller or audio device.
#include "common/virtualMemory.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "kernel/memory.h"
#include "loader/guestExecution.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    Common::VirtualMemory::Init();
    if (argc == 2 && std::strcmp(argv[1], "--memory-only") == 0) {
        Common::InitializeThreads();
        Common::Subsystems subsystems;
        subsystems.Initialize<Config::Lifecycle>();
        Config::ConfigOptions options;
        options.printf_direction = Config::LogDirection::Silent;
        Config::Load(options);
        subsystems.Initialize<Log::Lifecycle>();
        subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
        const bool pass = Loader::GuestExecution::MemoryAccessSelfTest();
        std::printf("PS5_ANDROID_FULL_ENGINE_MEMORY_TEST=%s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    const bool pass = Loader::GuestExecution::RuntimeSelfTest();
    std::printf("PS5_ANDROID_FULL_RUNTIME_SELF_TEST=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
