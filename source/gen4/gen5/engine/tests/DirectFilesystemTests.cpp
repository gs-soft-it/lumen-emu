// SPDX-License-Identifier: GPL-2.0-only
// Exercise production filesystem and libc exports through the direct FEX ABI.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/guestExecution.h"
#include "loader/runtimeLinker.h"
#include "loader/timer.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unistd.h>

namespace {
namespace Kernel = Libs::LibKernel;
namespace Memory = Kernel::Memory;
namespace FileSystem = Kernel::FileSystem;
using Mode = Common::VirtualMemory::Mode;

void Check(bool success, const char* name) {
    std::printf("DIRECT_FILESYSTEM_TEST %s %s\n", success ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!success) std::abort();
}

uint64_t Invoke(const char* nid, std::initializer_list<uint64_t> args = {}) {
    auto* symbols = Common::Singleton<Loader::RuntimeLinker>::Instance()->Symbols();
    const auto* symbol = symbols->FindByNid(nid, Loader::SymbolType::Func);
    uint64_t result {};
    if (symbol == nullptr || !Loader::GuestExecution::Invoke(
            symbol->vaddr, args.begin(), args.size(), &result)) {
        Check(false, "production export returns through direct FEX");
    }
    return result;
}

void CheckLibc(uint64_t memory) {
    auto* bytes = reinterpret_cast<char*>(memory);
    std::memcpy(bytes, "abcx\0abcy", 10);
    Check(static_cast<int32_t>(Invoke("Ovb2dSJOAuE", {memory, memory + 5})) < 0 &&
          static_cast<int32_t>(Invoke("Ovb2dSJOAuE", {memory + 5, memory})) > 0,
          "strcmp NID compares complete strings in both directions");
    Check(Invoke("aesyjrHVWy4", {memory, memory + 5, 3}) == 0 &&
          static_cast<int32_t>(Invoke("aesyjrHVWy4", {memory, memory + 5, 4})) < 0 &&
          Invoke("aesyjrHVWy4", {memory, memory + 5, 0}) == 0,
          "strncmp NID preserves the explicit bound and zero length");
    Check(Invoke("kiZSXIWd9vg", {memory + 32, memory}) == memory + 32 &&
          std::strcmp(bytes + 32, "abcx") == 0,
          "strcpy NID has the two-argument ABI and returns its destination");
    auto* symbols = Common::Singleton<Loader::RuntimeLinker>::Instance()->Symbols();
    Check(symbols->Find({"5Xa2ACNECdo", "LibcInternal", 1, "LibcInternal", 1, 1,
                         Loader::SymbolType::Func}) == nullptr,
          "strcpy no longer occupies the incompatible strcpy_s NID");
}

void CheckFilesystem(uint64_t memory, const std::filesystem::path& directory) {
    auto* bytes = reinterpret_cast<char*>(memory);
    std::memcpy(bytes, "guest\n", 6);
    FileSystem::Initialize();
    FileSystem::Mount(directory, "/savedata0");
    Check(static_cast<int32_t>(Invoke("juWbTNM+8hw", {uint64_t(-1)})) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF &&
          static_cast<int32_t>(Invoke("fTx66l5iWIA", {uint64_t(-1)})) ==
              Kernel::KERNEL_ERROR_EBADF,
          "POSIX and kernel fsync report their respective invalid-descriptor errors");

    const int fd = FileSystem::KernelOpen("/savedata0/fsync.dat", 0x602, 0600);
    Check(fd >= 3 && Invoke("FxVZqBAA7ks", {uint64_t(fd), memory, 6}) == 6 &&
          Invoke("juWbTNM+8hw", {uint64_t(fd)}) == 0 &&
          FileSystem::KernelLseek(fd, 0, 1) == 6,
          "translated write and fsync preserve the file offset");
    FILE* persisted = std::fopen((directory / "fsync.dat").c_str(), "rb");
    std::array<char, 7> content {};
    Check(persisted != nullptr && std::fread(content.data(), 1, 6, persisted) == 6 &&
          std::memcmp(content.data(), "guest\n", 6) == 0,
          "fsync makes buffered bytes visible to a separate host reader");
    std::fclose(persisted);
    Check(FileSystem::KernelClose(fd) == 0 &&
          static_cast<int32_t>(Invoke("juWbTNM+8hw", {uint64_t(fd)})) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
          "fsync on a closed descriptor reports EBADF");
    Check(static_cast<int64_t>(Invoke("FxVZqBAA7ks", {uint64_t(-1), memory, 6})) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
          "translated write returns EBADF instead of aborting on negative descriptors");

    FILE* capture = std::fopen((directory / "stdout.dat").c_str(), "w+b");
    Check(capture != nullptr, "create stdout capture");
    std::fflush(stdout);
    const int saved_stdout = dup(STDOUT_FILENO);
    Check(saved_stdout >= 0, "retain stdout descriptor");
    const bool redirected = dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO;
    const auto written = redirected ? Invoke("FxVZqBAA7ks", {1, memory, 6}) : 0;
    std::fflush(stdout);
    const bool restored = dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO;
    close(saved_stdout);
    Check(restored, "restore stdout descriptor");
    Check(redirected && std::fseek(capture, 0, SEEK_SET) == 0 && written == 6 &&
          std::fread(content.data(), 1, 6, capture) == 6 &&
          std::memcmp(content.data(), "guest\n", 6) == 0,
          "translated stdout write publishes the bytes instead of discarding them");
    std::fclose(capture);
    FileSystem::Shutdown();
}
} // namespace

int main(int argc, char** argv) {
    Common::InitializeThreads();
    Config::Initialize();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    Log::Initialize();
    Loader::Timer::Start();
    Kernel::Initialize();
    Memory::Initialize();
    Kernel::PthreadInitSelfForMainThread();
    Kernel::PthreadCreateMainGuestStack();
    Check(Loader::GuestExecution::InitializeRuntime(), "production direct engine bootstrap");
    auto* runtime = Common::Singleton<Loader::RuntimeLinker>::Instance();
    Libs::InitAll(runtime->Symbols());
    Check(Loader::GuestExecution::PrepareRuntime(), "production imports registered");
    const auto memory = Memory::AllocateRuntimeMemory(0, 0x4000, Mode::ReadWrite,
                                                     "filesystem fixture");
    Check(memory != 0, "allocate guest strings");
    CheckLibc(memory);

    const std::filesystem::path parent = argc > 1 ? argv[1] : ".";
    std::string pattern = (parent / "lumen-filesystem-XXXXXX").string();
    Check(mkdtemp(pattern.data()) != nullptr, "create isolated filesystem fixture");
    CheckFilesystem(memory, pattern);
    std::error_code error;
    std::filesystem::remove_all(pattern, error);
    Check(!error, "remove only the fixture's temporary files");
    std::printf("DIRECT_FILESYSTEM_TEST PASS all\n");
    std::fflush(stdout);
    std::quick_exit(0);
}
