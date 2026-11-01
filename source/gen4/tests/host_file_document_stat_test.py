#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check production HostFile::Stat using real POSIX file descriptors.

Only the mount/provider boundary and IOFile shell are substituted. The test
does not measure timings or exercise Android JNI. Its negative control removes
the new descriptor branch and must fail the same assertions.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    begin = source.index(signature)
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


PRELUDE = r'''
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#define __linux__ 1
#endif
#define __ANDROID__ 1
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (false)
using u64 = std::uint64_t;
using s64 = std::int64_t;
namespace fs = std::filesystem;
const fs::path document_path = "/document-tree/game/asset.bin";
unsigned provider_queries{}, size_queries{};
namespace Lumen::Documents {
bool IsMounted(const fs::path& path) { return path == document_path; }
}
namespace Lumen::HostFileSystem {
bool stat(const fs::path& path, struct stat& result) {
    ++provider_queries;
    return ::stat(path.c_str(), &result) == 0;
}
}
struct FileStat {
    u64 size{};
    bool is_directory{};
    s64 mtime_sec{}, mtime_nsec{}, atime_sec{}, atime_nsec{}, ctime_sec{}, ctime_nsec{};
};
struct TestFile {
    int fd;
    int GetNativeDescriptor() const { return fd; }
};
class HostFile {
public:
    fs::path m_path;
    TestFile m_file;
    u64 Size() {
        ++size_queries;
        struct stat st{};
        return ::fstat(m_file.fd, &st) == 0 ? st.st_size : 0;
    }
    void Stat(FileStat& out);
};
bool matches(const FileStat& got, int fd) {
    struct stat expected{};
    if (::fstat(fd, &expected) != 0) return false;
    return got.size == static_cast<u64>(expected.st_size) && !got.is_directory &&
        got.mtime_sec == expected.st_mtim.tv_sec && got.mtime_nsec == expected.st_mtim.tv_nsec &&
        got.atime_sec == expected.st_atim.tv_sec && got.atime_nsec == expected.st_atim.tv_nsec &&
        got.ctime_sec == expected.st_ctim.tv_sec && got.ctime_nsec == expected.st_ctim.tv_nsec;
}
'''

TEST = r'''
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const fs::path backing = fs::path(argv[1]) / "backing.bin";
    const fs::path renamed = fs::path(argv[1]) / "renamed.bin";
    int writer = ::open(backing.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(writer >= 0);
    CHECK(::write(writer, "abcdef", 6) == 6);
    const timespec times[2]{{1700000000, 123456000}, {1700000001, 987654000}};
    CHECK(::futimens(writer, times) == 0);
    int reader = ::open(backing.c_str(), O_RDONLY);
    CHECK(reader >= 0);
    CHECK(::lseek(reader, 2, SEEK_SET) == 2);
    HostFile document{document_path, {reader}};
    FileStat got{};
    document.Stat(got);
    CHECK(matches(got, reader));
    CHECK(got.size == 6 && got.mtime_sec == times[1].tv_sec);
    CHECK(provider_queries == 0 && size_queries == 0);
    CHECK(::lseek(reader, 0, SEEK_CUR) == 2);

    CHECK(::rename(backing.c_str(), renamed.c_str()) == 0);
    int replacement = ::open(backing.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(replacement >= 0 && ::write(replacement, "xx", 2) == 2);
    document.Stat(got);
    CHECK(matches(got, reader) && got.size == 6);
    CHECK(::unlink(renamed.c_str()) == 0);
    CHECK(::ftruncate(writer, 3) == 0);
    document.Stat(got);
    CHECK(matches(got, reader) && got.size == 3);

    const u64 large_size = (u64{1} << 32) + 17;
    CHECK(::ftruncate(writer, static_cast<off_t>(large_size)) == 0);
    document.Stat(got);
    CHECK(matches(got, reader) && got.size == large_size);
    CHECK(provider_queries == 0 && size_queries == 0);
    CHECK(::lseek(reader, 0, SEEK_CUR) == 2);

    int pipes[2];
    CHECK(::pipe(pipes) == 0);
    HostFile pipe_file{document_path, {pipes[0]}};
    got.size = 999;
    got.mtime_sec = 999;
    pipe_file.Stat(got);
    CHECK(got.size == 0 && got.mtime_sec == 0 && !got.is_directory);
    HostFile invalid{document_path, {-1}};
    got.size = 999;
    invalid.Stat(got);
    CHECK(got.size == 0 && provider_queries == 0 && size_queries == 0);

    // An ordinary path still follows the unchanged backend implementation.
    HostFile ordinary{backing, {replacement}};
    ordinary.Stat(got);
    CHECK(matches(got, replacement) && got.size == 2);
    CHECK(size_queries == 1 && provider_queries == 1);
    CHECK(::close(reader) == 0 && ::close(writer) == 0 && ::close(replacement) == 0);
    CHECK(::close(pipes[0]) == 0 && ::close(pipes[1]) == 0);
    CHECK(::unlink(backing.c_str()) == 0);
    std::puts("PASS: live fd stat; zero provider/Size queries; timestamps, position, replacement, unlink, resize >4GiB, invalid and nonregular fd; ordinary route unchanged");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1]
                        / "src/core/file_sys/backends/host_fs.cpp")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    body = function(args.source.read_text(), "void HostFile::Stat(FileStat& out)")
    begin = body.index("#if defined(__ANDROID__)")
    end = body.index("#endif", begin) + len("#endif")
    previous = body[:begin] + body[end:]
    with tempfile.TemporaryDirectory(prefix="gen4-document-stat-") as temporary:
        root = Path(temporary)
        for label, tested in [("current", body), ("without-descriptor-fix", previous)]:
            directory = root / label
            directory.mkdir()
            source, binary = directory / "test.cpp", directory / "test"
            source.write_text(PRELUDE + "\n" + tested + "\n" + TEST)
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(directory)], capture_output=True, text=True)
            if label == "current":
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print(result.stdout.strip())
            else:
                if result.returncode == 0:
                    raise RuntimeError("Negative control unexpectedly passed")
                print("PASS: previous path-based implementation rejected: " + result.stderr.strip())


if __name__ == "__main__":
    main()
