// SPDX-License-Identifier: MIT
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/orbis_error.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace Libraries::Kernel {
s32 PS4_SYSV_ABI sceKernelFcntl(s32 fd, s32 command, u64 argument);
s32 PS4_SYSV_ABI sceKernelChmod(const char* path, u32 mode);
s32 PS4_SYSV_ABI sceKernelFchmod(s32 fd, u32 mode);
s32 PS4_SYSV_ABI sceKernelUtimes(const char* path, const OrbisKernelTimeval* times);
s32 PS4_SYSV_ABI sceKernelFutimes(s32 fd, const OrbisKernelTimeval* times);
}
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"metadata contract failed line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
int main() {
    namespace fs = std::filesystem;
    using namespace Libraries::Kernel;
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(std::random_device{}());
    const auto directory = fs::temp_directory_path() / ("lumen-file-contract-" + nonce);
    fs::create_directories(directory);
    Common::FS::InitializeUserPaths(directory / "settings");
    Common::Log::Setup("file-contract.log");
    std::ofstream(directory / "file", std::ios::binary) << "metadata";
    auto* mounts = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    mounts->Mount(directory, "/contract");
    mounts->Mount(directory, "/readonly", true);
    handles->CreateStdHandles();
    const auto fd = posix_open("/contract/file", ORBIS_KERNEL_O_RDWR, 0600);
    REQUIRE(fd >= 3);
    REQUIRE(sceKernelFcntl(fd, 1, 0) == 0);
    REQUIRE(sceKernelFcntl(fd, 2, 1) == 0 && sceKernelFcntl(fd, 1, 0) == 1);
    REQUIRE(sceKernelFcntl(fd, 2, 2) == ORBIS_KERNEL_ERROR_EINVAL);
    const auto flags = sceKernelFcntl(fd, 3, 0);
    REQUIRE((flags & 3) == ORBIS_KERNEL_O_RDWR);
    REQUIRE(sceKernelFcntl(fd, 4, flags | ORBIS_KERNEL_O_NONBLOCK) == 0);
    REQUIRE(sceKernelFcntl(fd, 3, 0) & ORBIS_KERNEL_O_NONBLOCK);
    REQUIRE(sceKernelChmod("/contract/file", 0400) == 0);
    OrbisKernelStat stat{};
    REQUIRE(QueryFileMetadata("/contract/file", stat) == 0 && (stat.st_mode & 0600) == 0400);
    REQUIRE(sceKernelFchmod(fd, 0600) == 0);
    REQUIRE(QueryFileMetadata(fd, stat) == 0 && (stat.st_mode & 0600) == 0600 && stat.st_size == 8);
    const OrbisKernelTimeval times[2]{{1700000000,123456},{1700000010,654321}};
    REQUIRE(sceKernelUtimes("/contract/file", times) == 0);
    REQUIRE(QueryFileMetadata(fd, stat) == 0 && stat.st_atim.tv_sec == times[0].tv_sec && stat.st_mtim.tv_sec == times[1].tv_sec);
#ifndef _WIN32
    REQUIRE(stat.st_atim.tv_nsec == 123456000 && stat.st_mtim.tv_nsec == 654321000);
    fs::rename(directory / "file", directory / "renamed");
    REQUIRE(sceKernelFchmod(fd, 0400) == 0);
    REQUIRE(QueryFileMetadata(fd, stat) == 0 && (stat.st_mode & 0600) == 0400);
    REQUIRE(sceKernelFchmod(fd, 0600) == 0);
#endif
    REQUIRE(sceKernelFutimes(fd, times) == 0);
    REQUIRE(QueryFileMetadata(fd, stat) == 0 && stat.st_mtim.tv_sec == times[1].tv_sec);
    const OrbisKernelTimeval bad[2]{{0,-1},{0,0}};
    REQUIRE(sceKernelFutimes(fd, bad) == ORBIS_KERNEL_ERROR_EINVAL);
    REQUIRE(sceKernelChmod("/readonly/file", 0600) == ORBIS_KERNEL_ERROR_EROFS);
    REQUIRE(sceKernelUtimes("/readonly/file", times) == ORBIS_KERNEL_ERROR_EROFS);
    REQUIRE(posix_close(fd) == 0);
    REQUIRE(sceKernelFcntl(fd, 1, 0) == ORBIS_KERNEL_ERROR_EBADF);
    mounts->UnmountAll();
    Common::Log::Shutdown();
    fs::remove_all(directory);
    std::puts("file metadata and descriptor contracts PASS");
}
