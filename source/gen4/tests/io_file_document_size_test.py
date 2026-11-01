#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual IOFile::GetSize body with real POSIX descriptors.

Only mount lookup, the pathname provider and logging are substituted. The
provider deliberately fails after admission, so consulting it is a regression.
This checks descriptor semantics and query counts, not Android JNI or timings.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    begin = source.index(signature)
    opening = source.index("{", begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


PRELUDE = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define __ANDROID__ 1
#define LOG_ERROR(...) ((void)0)
using u64 = std::uint64_t;
namespace fs = std::filesystem;
const fs::path document_path = "/document-tree/game/asset.bin";
unsigned provider_queries = 0;
namespace Lumen::Documents {
bool IsMounted(const fs::path& path) { return path == document_path; }
}
namespace Lumen::HostFileSystem {
std::uintmax_t file_size(const fs::path& path, std::error_code& error) {
    if (Lumen::Documents::IsMounted(path)) {
        ++provider_queries;
        error = std::make_error_code(std::errc::permission_denied);
        return static_cast<std::uintmax_t>(-1);
    }
    return fs::file_size(path, error);
}
}
class IOFile {
public:
    FILE* file;
    fs::path file_path;
    bool IsOpen() const { return file != nullptr; }
    u64 GetSize() const;
};
'''


TEST = r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path backing = fs::path(argv[1]) / "backing.bin";
    FILE* writer = std::fopen(backing.c_str(), "w+b");
    assert(writer);
    assert(std::fwrite("abcdef", 1, 6, writer) == 6);
    assert(std::fflush(writer) == 0);
    FILE* reader = std::fopen(backing.c_str(), "rb");
    assert(reader);
    IOFile document{reader, document_path};
    assert(std::fgetc(reader) == 'a');
    assert(document.GetSize() == 6);
    assert(provider_queries == 0);
    assert(std::ftell(reader) == 1);
    assert(std::fgetc(reader) == 'b');

    // The open descriptor remains authoritative after removal of its name.
    assert(::unlink(backing.c_str()) == 0);
    assert(::ftruncate(::fileno(writer), 3) == 0);
    assert(document.GetSize() == 3);
    assert(provider_queries == 0);
    assert(std::ftell(reader) == 2);

    // No cached length or 32-bit truncation; this is a sparse file, not payload.
    const u64 large_size = (u64{1} << 32) + 17;
    assert(::ftruncate(::fileno(writer), static_cast<off_t>(large_size)) == 0);
    assert(document.GetSize() == large_size);
    assert(provider_queries == 0);
    assert(std::ftell(reader) == 2);

    // A descriptor that is no longer a regular file must fail without fallback.
    int pipes[2];
    assert(::pipe(pipes) == 0);
    assert(::dup2(pipes[0], ::fileno(reader)) >= 0);
    assert(document.GetSize() == 0);
    assert(provider_queries == 0);
    assert(::close(pipes[0]) == 0);
    assert(::close(pipes[1]) == 0);
    assert(::close(::fileno(reader)) == 0);
    assert(document.GetSize() == 0);
    assert(provider_queries == 0);
    std::fclose(reader); // Releases FILE state; its descriptor was closed above.
    assert(std::fclose(writer) == 0);

    // Ordinary files keep pathname semantics and flush buffered writes.
    const fs::path ordinary_path = fs::path(argv[1]) / "ordinary.bin";
    FILE* ordinary_file = std::fopen(ordinary_path.c_str(), "w+b");
    assert(ordinary_file);
    IOFile ordinary{ordinary_file, ordinary_path};
    assert(std::fwrite("normal", 1, 6, ordinary_file) == 6);
    assert(ordinary.GetSize() == 6);
    assert(::unlink(ordinary_path.c_str()) == 0);
    assert(ordinary.GetSize() == 0);
    assert(std::fclose(ordinary_file) == 0);
    IOFile closed{nullptr, document_path};
    assert(closed.GetSize() == 0);
    assert(provider_queries == 0);
    std::puts("PASS: document size uses live fd with zero provider queries; position, large sizes, errors and ordinary files preserved");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/common/io_file.cpp")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    body = function(args.source.read_text(), "u64 IOFile::GetSize() const")
    with tempfile.TemporaryDirectory(prefix="gen4-document-size-") as directory:
        directory = Path(directory)
        source = directory / "test.cpp"
        source.write_text(PRELUDE + "\n" + body + "\n" + TEST)
        binary = directory / "test"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(directory)], check=True)


if __name__ == "__main__":
    main()
