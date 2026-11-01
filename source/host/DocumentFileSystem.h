// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif
#if defined(__ANDROID__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Keep the independently linked console C++ runtimes behind a C ABI. No C++
// allocation, exception, or library-owned container crosses the DSO boundary.
#if defined(__ANDROID__)
extern "C" {
bool LumenDocumentIsMounted(const char* path);
void LumenDocumentBeginReadOperation();
void LumenDocumentEndReadOperation();
int LumenDocumentOpenRead(const char* path);
bool LumenDocumentStat(const char* path, struct stat* result);
int LumenDocumentList(const char* path, void (*entry)(void*, const char*), void* context);
int LumenDocumentListEntries(const char* path, void (*entry)(void*, const char*, bool), void* context);
}
#endif
namespace Lumen::Documents {
// Repeated existence/type/size checks inside a single syscall may reuse its
// successful metadata snapshot. Descriptors always receive live admission.
class ReadOperation final {
public:
    ReadOperation() {
#if defined(__ANDROID__)
        LumenDocumentBeginReadOperation();
#endif
    }
    ~ReadOperation() {
#if defined(__ANDROID__)
        LumenDocumentEndReadOperation();
#endif
    }
    ReadOperation(const ReadOperation&) = delete;
    ReadOperation& operator=(const ReadOperation&) = delete;
};

#if defined(__ANDROID__)
inline bool IsMounted(const std::filesystem::path& path) {
    return LumenDocumentIsMounted(path.c_str());
}
inline int OpenRead(const std::filesystem::path& path) {
    return LumenDocumentOpenRead(path.c_str());
}
inline bool Stat(const std::filesystem::path& path, struct stat& result) {
    return LumenDocumentStat(path.c_str(), &result);
}
inline std::vector<std::string> List(const std::filesystem::path& path, std::error_code& error) {
    std::vector<std::string> result;
    const int status = LumenDocumentList(path.c_str(), [](void* context, const char* name) {
        static_cast<std::vector<std::string>*>(context)->emplace_back(name);
    }, &result);
    error = status ? std::error_code(status, std::generic_category()) : std::error_code{};
    if (error) result.clear();
    return result;
}
struct DirectoryEntry {
    std::string name;
    bool is_file;
};
inline std::vector<DirectoryEntry> ListEntries(const std::filesystem::path& path,
                                              std::error_code& error) {
    std::vector<DirectoryEntry> result;
    const int status = LumenDocumentListEntries(path.c_str(),
        [](void* context, const char* name, bool is_file) {
            static_cast<std::vector<DirectoryEntry>*>(context)->push_back({name, is_file});
        }, &result);
    error = status ? std::error_code(status, std::generic_category()) : std::error_code{};
    if (error) result.clear();
    return result;
}
#else
inline bool IsMounted(const std::filesystem::path&) { return false; }
#endif
}

// The selected document tree is authoritative, including failures. Never fall
// through to a stale metadata cache for a path owned by that mount.
namespace Lumen::HostFileSystem {
namespace fs = std::filesystem;
inline void ReportError(const char* operation, const fs::path& path, std::error_code error) {
#if defined(__cpp_exceptions)
    throw fs::filesystem_error(operation, path, error);
#else
    (void)operation;
    (void)path;
    errno = error.value();
#endif
}
#if defined(__unix__) || defined(__APPLE__)
inline bool stat(const fs::path& path, struct stat& result) {
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) return Documents::Stat(path, result);
#endif
    return ::stat(path.c_str(), &result) == 0;
}
#endif
inline fs::file_time_type last_write_time(const fs::path& path, std::error_code& error) {
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) {
        struct stat info{};
        if (!Documents::Stat(path, info)) {
            error = std::error_code(errno, std::generic_category());
            return fs::file_time_type::min();
        }
        error.clear();
        const auto time = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::seconds(info.st_mtim.tv_sec) + std::chrono::nanoseconds(info.st_mtim.tv_nsec)));
        return fs::file_time_type::clock::now() + (time - std::chrono::system_clock::now());
    }
#endif
    return fs::last_write_time(path, error);
}
inline fs::file_time_type last_write_time(const fs::path& path) {
    std::error_code error;
    auto result = Lumen::HostFileSystem::last_write_time(path, error);
    if (error) ReportError("document time", path, error);
    return result;
}
inline fs::file_status symlink_status(const fs::path& path, std::error_code& error) {
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) {
        struct stat info{};
        if (!Documents::Stat(path, info)) {
            error = std::error_code(errno, std::generic_category());
            return fs::file_status(fs::file_type::not_found);
        }
        error.clear();
        return fs::file_status(S_ISDIR(info.st_mode) ? fs::file_type::directory : fs::file_type::regular,
                              fs::perms::owner_read);
    }
#endif
    return fs::symlink_status(path, error);
}
inline fs::file_status symlink_status(const fs::path& path) {
    std::error_code error;
    auto result = Lumen::HostFileSystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        ReportError("document status", path, error);
    return result;
}
inline bool exists(const fs::path& path, std::error_code& error) {
    if (!Documents::IsMounted(path)) return fs::exists(path, error);
    return fs::exists(Lumen::HostFileSystem::symlink_status(path, error));
}
inline bool exists(const fs::path& path) {
    if (!Documents::IsMounted(path)) return fs::exists(path);
    std::error_code error;
    return Lumen::HostFileSystem::exists(path, error);
}
inline bool is_directory(const fs::path& path, std::error_code& error) {
    if (!Documents::IsMounted(path)) return fs::is_directory(path, error);
    return fs::is_directory(Lumen::HostFileSystem::symlink_status(path, error));
}
inline bool is_directory(const fs::path& path) {
    if (!Documents::IsMounted(path)) return fs::is_directory(path);
    std::error_code error;
    return Lumen::HostFileSystem::is_directory(path, error);
}
inline bool is_regular_file(const fs::path& path, std::error_code& error) {
    if (!Documents::IsMounted(path)) return fs::is_regular_file(path, error);
    return fs::is_regular_file(Lumen::HostFileSystem::symlink_status(path, error));
}
inline bool is_regular_file(const fs::path& path) {
    if (!Documents::IsMounted(path)) return fs::is_regular_file(path);
    std::error_code error;
    return Lumen::HostFileSystem::is_regular_file(path, error);
}
inline uintmax_t file_size(const fs::path& path, std::error_code& error) {
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) {
        struct stat info{};
        if (!Documents::Stat(path, info) || !S_ISREG(info.st_mode)) {
            error = std::error_code(errno ? errno : EISDIR, std::generic_category());
            return static_cast<uintmax_t>(-1);
        }
        error.clear();
        return info.st_size;
    }
#endif
    return fs::file_size(path, error);
}
inline uintmax_t file_size(const fs::path& path) {
    std::error_code error;
    auto result = Lumen::HostFileSystem::file_size(path, error);
    if (error) ReportError("document size", path, error);
    return result;
}
inline fs::path canonical(const fs::path& path, std::error_code& error) {
    if (!Documents::IsMounted(path)) return fs::canonical(path, error);
    if (!Lumen::HostFileSystem::exists(path, error)) return {};
    return path.lexically_normal();
}
inline fs::path canonical(const fs::path& path) {
    std::error_code error;
    auto result = Lumen::HostFileSystem::canonical(path, error);
    if (error) ReportError("document path", path, error);
    return result;
}
inline FILE* fopen(const fs::path& path, const char* mode) {
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) {
        if (mode[0] != 'r' || std::string(mode).find('+') != std::string::npos) {
            errno = EROFS;
            return nullptr;
        }
        const int fd = Documents::OpenRead(path);
        if (fd < 0) return nullptr;
        FILE* result = ::fdopen(fd, mode);
        if (!result) ::close(fd);
        else errno = 0;
        return result;
    }
#endif
    return std::fopen(path.string().c_str(), mode);
}
// Reads the granted descriptor itself. Reopening /proc/self/fd would perform
// another inode permission check and is not equivalent to a SAF grant.
class InputStream : public std::istream {
public:
    explicit InputStream(const fs::path& path, std::ios::openmode mode = std::ios::in)
        : std::istream(nullptr) {
#if defined(__ANDROID__)
        if (Documents::IsMounted(path)) {
            opened_ = document_.Open(path, mode);
            rdbuf(&document_);
        } else
#endif
        {
            opened_ = native_.open(path, mode | std::ios::in) != nullptr;
            rdbuf(&native_);
        }
        if (!opened_) setstate(std::ios::failbit);
    }
    bool is_open() const { return opened_; }
private:
#if defined(__ANDROID__)
    class DocumentBuffer final : public std::streambuf {
    public:
        ~DocumentBuffer() override { if (file_) std::fclose(file_); }
        bool Open(const fs::path& path, std::ios::openmode mode) {
            file_ = Lumen::HostFileSystem::fopen(path, "rb");
            if (!file_) return false;
            return !(mode & std::ios::ate) || ::fseeko(file_, 0, SEEK_END) == 0;
        }
    protected:
        int_type underflow() override {
            if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
            if (!file_ || std::fread(&pending_, 1, 1, file_) != 1) return traits_type::eof();
            setg(&pending_, &pending_, &pending_ + 1);
            return traits_type::to_int_type(pending_);
        }
        std::streamsize xsgetn(char* output, std::streamsize size) override {
            if (!file_ || size <= 0) return 0;
            std::streamsize copied{};
            if (gptr() && gptr() < egptr()) {
                *output++ = *gptr(); gbump(1); --size; ++copied;
            }
            return copied + static_cast<std::streamsize>(std::fread(output, 1, size, file_));
        }
        pos_type seekoff(off_type offset, std::ios::seekdir direction,
                         std::ios::openmode) override {
            if (!file_) return pos_type(off_type(-1));
            const int origin = direction == std::ios::beg ? SEEK_SET
                : direction == std::ios::end ? SEEK_END : SEEK_CUR;
            if (origin == SEEK_CUR && gptr()) offset -= egptr() - gptr();
            if (::fseeko(file_, offset, origin) != 0) return pos_type(off_type(-1));
            setg(nullptr, nullptr, nullptr);
            return pos_type(::ftello(file_));
        }
        pos_type seekpos(pos_type position, std::ios::openmode mode) override {
            return seekoff(off_type(position), std::ios::beg, mode);
        }
    private:
        FILE* file_{};
        char pending_{};
    } document_;
#endif
    std::filebuf native_;
    bool opened_{};
};

inline std::vector<fs::path> directory_paths(const fs::path& path, std::error_code& error) {
    std::vector<fs::path> result;
#if defined(__ANDROID__)
    if (Documents::IsMounted(path)) {
        for (const auto& name : Documents::List(path, error)) result.push_back(path / name);
        return result;
    }
#endif
    for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
        result.push_back(it->path());
    return result;
}
inline std::vector<fs::path> directory_paths(const fs::path& path) {
    std::error_code error;
    auto result = directory_paths(path, error);
    if (error) ReportError("document directory", path, error);
    return result;
}
}
