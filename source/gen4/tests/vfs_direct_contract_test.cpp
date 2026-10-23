// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zarchive/zarchivewriter.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the direct virtual filesystem implementation.
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace Core {
class MemoryBackingAliasTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

namespace Libraries::Fios2 {
s32 PS4_SYSV_ABI sceFiosInitialize(const void*);
s32 PS4_SYSV_ABI sceFiosTerminate();
s32 PS4_SYSV_ABI sceFiosIOFilterAdd(s32, void*, void*);
s32 PS4_SYSV_ABI sceFiosIOFilterRemove(s32);
void* PS4_SYSV_ABI sceFiosIOFilterPsarcDearchiver();
s32 PS4_SYSV_ABI sceFiosDeleteSync(const void*, const char*);
s32 PS4_SYSV_ABI sceFiosFHSyncSync(const void*, s32);
s32 PS4_SYSV_ABI sceFiosDirectoryCreateSync(const void*, const char*);
s64 PS4_SYSV_ABI sceFiosFHGetSize(s32);
s32 PS4_SYSV_ABI sceFiosIsValidHandle(s32);
s32 PS4_SYSV_ABI sceFiosFHPread(const void*, s32, void*, s64, s64);
s32 PS4_SYSV_ABI sceFiosFHPwrite(const void*, s32, const void*, s64, s64);
s64 PS4_SYSV_ABI sceFiosOpGetActualCount(s32);
s32 PS4_SYSV_ABI sceFiosOpDelete(s32);
} // namespace Libraries::Fios2

namespace {
namespace fs = std::filesystem;
using namespace Core::FileSys;
using namespace Libraries::Kernel;

void WriteHost(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(file.good());
}

std::string ReadBackend(MntPoints& mounts, std::string_view path) {
    const auto data = mounts.ReadFile(path);
    CHECK(data.has_value());
    return {data->begin(), data->end()};
}

void CheckDirectory(MntPoints& mounts, std::string_view path,
                    const std::set<std::string>& expected) {
    auto dir = mounts.OpenDir(path);
    CHECK(dir != nullptr);
    for (int pass = 0; pass < 2; ++pass) {
        std::set<std::string> names;
        DirEntry entry;
        while (dir->Next(entry)) CHECK(names.insert(entry.name).second);
        CHECK(names == expected);
        dir->Rewind();
    }
}

// Exercise the actual backend Map implementation using real anonymous/native
// mappings. The callback boundary excludes the unrelated GPU/VMM setup.
void CheckMap(IFile& file, std::string_view expected, bool native) {
    const auto page = static_cast<u64>(sysconf(_SC_PAGESIZE));
    const u64 length = native ? page : ((expected.size() + page - 1) / page + 1) * page;
    auto* const destination = static_cast<u8*>(mmap(nullptr, length, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(destination != MAP_FAILED);
    unsigned native_calls{}, anonymous_calls{}, protect_calls{};
    FileMapContext context{};
    if (auto* host = file.GetHostFile()) context.mapping_handle = host->GetFileMapping();
    context.map_native = [&](u8* addr, u64 size, u64 offset, u32 prot, uintptr_t descriptor) {
        ++native_calls;
        CHECK(prot == static_cast<u32>(Core::MemoryProt::CpuRead));
        CHECK(mmap(addr, size, PROT_READ, MAP_SHARED | MAP_FIXED,
                   static_cast<int>(descriptor), static_cast<off_t>(offset)) == addr);
    };
    context.map_anonymous = [&](u8* addr, u64 size) {
        ++anonymous_calls;
        CHECK(mmap(addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == addr);
    };
    context.protect = [&](u8* addr, u64 size, u32 prot) {
        ++protect_calls;
        CHECK(prot == static_cast<u32>(Core::MemoryProt::CpuRead));
        CHECK(mprotect(addr, size, PROT_READ) == 0);
    };
    const auto position = file.Tell();
    CHECK(file.Map(destination, length, 0, static_cast<u32>(Core::MemoryProt::CpuRead), context));
    CHECK(file.Tell() == position);
    CHECK(std::memcmp(destination, expected.data(), std::min<u64>(length, expected.size())) == 0);
    if (native) {
        CHECK(native_calls == 1 && anonymous_calls == 0 && protect_calls == 0);
    } else {
        CHECK(native_calls == 0 && anonymous_calls == 1 && protect_calls == 1);
        CHECK(std::all_of(destination + expected.size(), destination + length,
                          [](u8 byte) { return byte == 0; }));
    }
    CHECK(munmap(destination, length) == 0);
}

void CheckKernelFile(std::string_view path, std::string_view expected) {
    const std::string name{path};
    const s32 fd = posix_open(name.c_str(), ORBIS_KERNEL_O_RDONLY, 0);
    CHECK(fd >= 0);
    std::vector<char> bytes(expected.size() + 8, '\x5a');
    CHECK(sceKernelRead(fd, bytes.data(), bytes.size()) == static_cast<s64>(expected.size()));
    CHECK(std::string_view(bytes.data(), expected.size()) == expected);
    CHECK(std::all_of(bytes.begin() + expected.size(), bytes.end(),
                      [](char byte) { return byte == '\x5a'; }));
    CHECK(sceKernelRead(fd, bytes.data(), 1) == 0);
    CHECK(posix_lseek(fd, 1, 0) == 1);
    CHECK(sceKernelPread(fd, bytes.data(), 3, 2) == 3);
    CHECK(std::string_view(bytes.data(), 3) == expected.substr(2, 3));
    CHECK(posix_lseek(fd, 0, 1) == 1);

    OrbisKernelStat by_path{}, by_handle{};
    CHECK(sceKernelStat(name.c_str(), &by_path) == ORBIS_OK);
    CHECK(sceKernelFstat(fd, &by_handle) == ORBIS_OK);
    CHECK(by_path.st_size == static_cast<s64>(expected.size()));
    CHECK(by_handle.st_size == by_path.st_size);
    CHECK(by_handle.st_mode == (0100000 | 0777));
    CHECK(by_handle.st_blksize == 512);
    CHECK(by_handle.st_blocks == (by_handle.st_size + 511) / 512);
    CHECK(by_handle.st_mtim.tv_sec == by_path.st_mtim.tv_sec);
    CHECK(by_handle.st_mtim.tv_nsec == by_path.st_mtim.tv_nsec);
    CHECK(Libraries::Fios2::sceFiosFHGetSize(fd) == by_path.st_size);
    CHECK(Libraries::Fios2::sceFiosIsValidHandle(fd) == 1);
    CHECK(posix_close(fd) == ORBIS_OK);
}

void CheckFios(MntPoints& mounts, const fs::path& root) {
    using namespace Libraries::Fios2;
    const auto base = root / "fios";
    WriteHost(base / "data", "fios-original");
    mounts.Mount(base, "/fios");
    CHECK(sceFiosInitialize(nullptr) == 0);
    auto* filter = sceFiosIOFilterPsarcDearchiver();
    CHECK(filter != nullptr && filter == sceFiosIOFilterPsarcDearchiver());
    CHECK(sceFiosIOFilterAdd(-1, filter, nullptr) == 0);
    CHECK(sceFiosIOFilterRemove(-1) == 0);
    CHECK(sceFiosDeleteSync(nullptr, "/fios/missing") == 0);
    CHECK(sceFiosDeleteSync(nullptr, nullptr) == -1);
    CHECK(sceFiosFHSyncSync(nullptr, -1) == 0);
    CHECK(sceFiosDirectoryCreateSync(nullptr, "/fios/accepted") == 0);
    CHECK(!fs::exists(base / "accepted"));
    CHECK(sceFiosDirectoryCreateSync(nullptr, nullptr) == -1);

    const auto fd = posix_open("/fios/data", ORBIS_KERNEL_O_RDWR, 0);
    CHECK(fd >= 0);
    struct OpAttr {
        s64 deadline{};
        void* callback{};
        void* context{};
        s32 flags{};
        u32 tag{};
        void* user{};
        void* reserved{};
    };
    static_assert(sizeof(OpAttr) == 0x30);
    unsigned callbacks{};
    const auto callback = +[](void* context, s32, s32, s64) {
        ++*static_cast<unsigned*>(context);
    };
    const OpAttr attr{.callback = reinterpret_cast<void*>(callback), .context = &callbacks};
    CHECK(posix_lseek(fd, 3, 0) == 3);
    const auto write_op = sceFiosFHPwrite(&attr, fd, "X", 1, 1);
    CHECK(write_op > 0 && sceFiosOpGetActualCount(write_op) == 1);
    CHECK(posix_lseek(fd, 0, 1) == 3);
    CHECK(callbacks == 0);
    // The original fallback exposes the failed posix_lseek result (-1).
    CHECK(sceFiosFHPwrite(&attr, -1, "X", 1, 0) == -1);
    CHECK(callbacks == 0);
    std::array<char, 4> bytes{};
    const auto read_op = sceFiosFHPread(nullptr, fd, bytes.data(), bytes.size(), 0);
    CHECK(read_op > 0 && sceFiosOpGetActualCount(read_op) == 4);
    CHECK(std::string_view(bytes.data(), bytes.size()) == "fXos");
    CHECK(sceFiosTerminate() == 0);
    CHECK(sceFiosOpGetActualCount(read_op) == 4);
    CHECK(sceFiosOpGetActualCount(write_op) == 1);
    CHECK(sceFiosOpDelete(read_op) == 0 && sceFiosOpDelete(write_op) == 0);
    CHECK(sceFiosOpGetActualCount(read_op) == 0);
    CHECK(posix_close(fd) == 0);
    mounts.UnmountAll();
}

void CheckHost(MntPoints& mounts, const fs::path& root) {
    const auto base = root / "base";
    const std::string payload(8192, 'b');
    WriteHost(base / "common", "base-value");
    WriteHost(base / "host.bin", payload);
    WriteHost(base / "folder" / "base-only", "base");
    WriteHost(root / "base-UPDATE" / "common", "patch-value");
    WriteHost(root / "base-UPDATE" / "folder" / "patch-only", "patch");
    WriteHost(root / "base-mods" / "common", "mods-value");
    WriteHost(root / "base-mods" / "folder" / "mod-only", "mod");
    mounts.Mount(base, "/app0");
    CHECK(ReadBackend(mounts, "/app0/common") == "mods-value");
    CHECK(ReadBackend(mounts, "/app0//host.bin") == payload);
    CHECK(ReadBackend(mounts, "/app0/HOST.BIN") == payload);
    CHECK(!mounts.Exists("/app00/host.bin"));
    CheckDirectory(mounts, "/app0/folder", {"base-only", "patch-only", "mod-only"});
    CheckKernelFile("/app0/common", "mods-value");
    CheckKernelFile("/app0/host.bin", payload);
    auto host = mounts.Open("/app0/host.bin");
    CHECK(host && host->GetMmapPolicy() == MmapPolicy::Native);
    CheckMap(*host, payload, true);
    FileStat backend_stat{};
    host->Stat(backend_stat);
    struct stat native_stat{};
    CHECK(::stat((base / "host.bin").c_str(), &native_stat) == 0);
    CHECK(backend_stat.size == static_cast<u64>(native_stat.st_size));
#ifdef __APPLE__
    CHECK(backend_stat.mtime_sec == native_stat.st_mtimespec.tv_sec);
    CHECK(backend_stat.mtime_nsec == native_stat.st_mtimespec.tv_nsec);
#else
    CHECK(backend_stat.mtime_sec == native_stat.st_mtim.tv_sec);
    CHECK(backend_stat.mtime_nsec == native_stat.st_mtim.tv_nsec);
#endif

    const auto fd = posix_open("/app0/created", ORBIS_KERNEL_O_CREAT | ORBIS_KERNEL_O_RDWR, 0666);
    CHECK(fd >= 0);
    CHECK(sceKernelWrite(fd, "written", 7) == 7);
    CHECK(sceKernelFsync(fd) == ORBIS_OK);
    CHECK(posix_close(fd) == ORBIS_OK);
    CHECK(ReadBackend(mounts, "/app0/created") == "written");
    OrbisKernelStat directory{};
    CHECK(sceKernelStat("/app0/folder", &directory) == ORBIS_OK);
    CHECK(directory.st_mode == (0040000 | 0777) && directory.st_size == 65536);

    mounts.Mount(base, "/read-only", true);
    CHECK(!mounts.Open("/read-only/host.bin", true));
    CHECK(posix_open("/read-only/host.bin", ORBIS_KERNEL_O_WRONLY, 0) < 0);
    CHECK(posix_open("/app0/missing", ORBIS_KERNEL_O_RDONLY, 0) < 0);
    mounts.UnmountAll();

    // A patch is used when the higher-priority mod is absent; the flag drops
    // only the patch layer, matching the original mount construction.
    fs::remove_all(root / "base-mods");
    mounts.Mount(base, "/app0");
    CHECK(ReadBackend(mounts, "/app0/common") == "patch-value");
    mounts.UnmountAll();
    MntPoints::ignore_game_patches = true;
    mounts.Mount(base, "/app0");
    CHECK(ReadBackend(mounts, "/app0/common") == "base-value");
    mounts.UnmountAll();
    MntPoints::ignore_game_patches = false;
}

void CheckArchive(MntPoints& mounts, const fs::path& root) {
    const auto archive_path = root / "archive.zar";
    std::string payload(10003, '\0');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i * 37);
    std::ofstream output{archive_path, std::ios::binary};
    CHECK(output.is_open());
    {
        ZArchiveWriter writer{
            [](int32_t part, void*) { CHECK(part == -1); },
            [](const void* data, size_t size, void* context) {
                auto& stream = *static_cast<std::ofstream*>(context);
                stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
                CHECK(stream.good());
            }, &output};
        CHECK(writer.MakeDir("nested"));
        CHECK(writer.StartNewFile("nested/item.bin"));
        writer.AppendData(payload.data(), payload.size());
        writer.Finalize();
    }
    output.close();
    mounts.Mount(archive_path, "/archive", true);
    CHECK(mounts.IsDirectory("/archive/nested"));
    CheckDirectory(mounts, "/archive/nested", {"item.bin"});
    CHECK(ReadBackend(mounts, "/archive/nested/item.bin") == payload);
    CheckKernelFile("/archive/nested/item.bin", payload);
    auto file = mounts.Open("/archive/nested/item.bin");
    CHECK(file && file->GetHostFile() == nullptr && file->GetMmapPolicy() == MmapPolicy::Copy);
    CHECK(file->Write("x", 1) == -1);
    CHECK(!mounts.Open("/archive/nested/item.bin", true));
    CheckMap(*file, payload, false);
    file.reset();
    mounts.UnmountAll();
}
} // namespace

int main(int argc, char** argv) {
    alarm(30);
    const fs::path parent = argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path();
    std::string pattern = (parent / "lumen-vfs-contract-XXXXXX").string();
    CHECK(mkdtemp(pattern.data()) != nullptr);
    const fs::path root{pattern};
    Common::FS::InitializeUserPaths(root / "user");
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("vfs-direct-contract.log");
    Core::MemoryBackingAliasTest::InitializeBootMetadata();
    auto* mounts = Common::Singleton<MntPoints>::Instance();
    Common::Singleton<HandleTable>::Instance()->CreateStdHandles();
    CheckHost(*mounts, root);
    CheckArchive(*mounts, root);
    CheckFios(*mounts, root);
    Common::Log::Shutdown();
    fs::remove_all(root);
    std::puts("VFS DIRECT contracts PASS: overlays, merged directories, case lookup, host/archive reads, writes, stat/fstat, read-only failures, real backend file mappings and original Fios boundaries");
}
