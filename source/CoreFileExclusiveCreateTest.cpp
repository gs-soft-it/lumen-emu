#include "Lumen/Core/File.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
#include <sys/stat.h>
#endif

namespace {
using File = Lumen::Core::File;
namespace fs = std::filesystem;

bool Check(bool value, const char* message)
{
	if (!value) std::fprintf(stderr, "CoreFileExclusiveCreateTest: FAIL %s\n", message);
	return value;
}

Lumen::String Name(const fs::path& path)
{
	const auto utf8 = path.u8string();
	return Lumen::String::FromUtf8(reinterpret_cast<const char*>(utf8.c_str()));
}

struct OpenFile
{
	File file;
	~OpenFile() { file.Close(); }
};

bool HasBytes(File& file, const std::array<uint8_t, 8>& expected)
{
	std::array<uint8_t, 8> actual {};
	uint32_t bytes = 0;
	return file.Size() == expected.size() && file.ReadAt(actual.data(), actual.size(), 0, &bytes) &&
	       bytes == actual.size() && actual == expected;
}

bool Run(const fs::path& directory)
{
	const std::array<uint8_t, 8> payload {0, 1, 17, 33, 127, 128, 254, 255};
	const auto existing = directory / "existing.bin";
#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
	// No test worker exists yet. Snapshot and restore the process mask before
	// native creation, then verify precisely the permissions it should apply.
	const mode_t mask = ::umask(0);
	::umask(mask);
#endif
	int error = -1;
	OpenFile original;
	if (!Check(original.file.CreateExclusive(Name(existing), File::Mode::ReadWrite, 0600, &error) && error == 0 &&
	           !original.file.IsInvalid() && original.file.Tell() == 0 && original.file.Size() == 0,
	           "new file returns the usable creation handle")) return false;
#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
	struct stat state {};
	if (!Check(::stat(existing.c_str(), &state) == 0 && (state.st_mode & 0777u) == (0600u & ~mask),
	           "native creation applies requested permissions and process umask")) return false;
#endif
	uint32_t written = 0;
	original.file.Write(payload.data(), payload.size(), &written);
	// No Flush: native positional reads must observe this completed Write.
	// Neither Seek nor Size may run first: they would flush a buffered stream
	// and conceal a broken Write -> pread interaction.
	std::array<uint8_t, 8> immediate {};
	uint32_t immediate_bytes = 0;
	if (!Check(written == payload.size() && original.file.ReadAt(immediate.data(), immediate.size(), 0, &immediate_bytes) &&
	           immediate_bytes == payload.size() && immediate == payload && original.file.Tell() == payload.size(),
	           "immediate positional read sees completed Write without flushing operation")) return false;
	if (!Check(original.file.Seek(3) && HasBytes(original.file, payload) &&
	           original.file.Tell() == 3, "write/read-at bytes and independent cursor without flush")) return false;
	for (int attempt = 0; attempt < 16; ++attempt)
	{
		OpenFile rejected;
		error = -1;
		if (!Check(!rejected.file.CreateExclusive(Name(existing), File::Mode::ReadWrite, 0600, &error) &&
		           error == EEXIST && rejected.file.IsInvalid() && HasBytes(original.file, payload) &&
		           original.file.Tell() == 3, "existing file rejected without truncation or handle/cursor changes")) return false;
	}
	original.file.Close();
	OpenFile reopened;
	if (!Check(reopened.file.Open(Name(existing), File::Mode::ReadWrite) && HasBytes(reopened.file, payload),
	           "close and reopen preserves existing contents")) return false;
	reopened.file.Close();

	// A pathname replacement cannot redirect the returned creation handle.
	const auto moving = directory / "moving.bin";
	const auto moved = directory / "moved.bin";
	OpenFile retained;
	if (!retained.file.CreateExclusive(Name(moving), File::Mode::ReadWrite, 0600, &error)) return false;
	std::error_code ec;
	fs::rename(moving, moved, ec);
	if (!Check(!ec, "rename the created file while retaining its handle")) return false;
	OpenFile replacement;
	if (!replacement.file.CreateExclusive(Name(moving), File::Mode::ReadWrite, 0600, &error)) return false;
	const std::array<uint8_t, 8> other {9, 9, 9, 9, 9, 9, 9, 9};
	replacement.file.Write(other.data(), other.size(), &written);
	retained.file.Write(payload.data(), payload.size(), &written);
	if (!Check(written == payload.size() && HasBytes(retained.file, payload) && HasBytes(replacement.file, other),
	           "all I/O retains file identity after rename/replacement")) return false;
	retained.file.Close();
	replacement.file.Close();

	for (const auto mode: {File::Mode::Read, File::Mode::Write, File::Mode::ReadWrite, File::Mode::WriteRead})
	{
		const auto name = directory / ("access-" + std::to_string(static_cast<int>(mode)));
		OpenFile file;
		if (!file.file.CreateExclusive(Name(name), mode, 0600, &error)) return false;
		written = 99;
		file.file.Write(payload.data(), payload.size(), &written);
		const bool readable = mode != File::Mode::Write;
		const bool writable = mode != File::Mode::Read;
		std::array<uint8_t, 8> actual {};
		uint32_t read = 99;
		const bool positioned = file.file.ReadAt(actual.data(), actual.size(), 0, &read);
		if (!Check(written == (writable ? payload.size() : 0) && positioned == readable &&
		           read == (readable && writable ? payload.size() : 0) &&
		           (!readable || !writable || actual == payload), "requested native access modes")) return false;
	}
	for (const auto& name: {directory / "missing-parent" / "file", directory})
	{
		OpenFile file;
		error = -1;
		if (!Check(!file.file.CreateExclusive(Name(name), File::Mode::ReadWrite, 0600, &error) && file.file.IsInvalid() &&
		           error == (name == directory ? EEXIST : ENOENT), "existing directory/missing parent errors leave no handle")) return false;
	}
	OpenFile invalid;
	const auto invalid_name = directory / "invalid-mode";
	if (!Check(!invalid.file.CreateExclusive(Name(invalid_name), static_cast<File::Mode>(-1), 0600, &error) &&
	           error == EINVAL && invalid.file.IsInvalid() && !fs::exists(invalid_name), "invalid mode has no file effects")) return false;

	const auto link = directory / "dangling-link";
	const auto absent_target = directory / "link-target";
	fs::create_symlink(absent_target, link, ec);
	if (ec)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
		std::puts("CoreFileExclusiveCreateTest: SKIP symlink fixture (host capability unavailable)");
#else
		return Check(false, "create dangling symlink fixture");
#endif
	} else
	{
		OpenFile file;
		if (!Check(!file.file.CreateExclusive(Name(link), File::Mode::ReadWrite, 0600, &error) && error == EEXIST &&
		           file.file.IsInvalid() && !fs::exists(absent_target) && fs::is_symlink(link), "dangling link rejected without following it")) return false;
	}

	constexpr unsigned workers = 8;
	for (unsigned round = 0; round < 8; ++round)
	{
		const auto name = directory / ("race-" + std::to_string(round));
		std::atomic<unsigned> ready {0};
		std::atomic<bool> start {false};
		std::array<bool, workers> created {};
		std::array<int, workers> errors {};
		std::array<std::thread, workers> threads;
		for (unsigned i = 0; i < workers; ++i)
		{
			threads[i] = std::thread([&, i] {
				OpenFile file;
				++ready;
				while (!start.load()) std::this_thread::yield();
				created[i] = file.file.CreateExclusive(Name(name), File::Mode::ReadWrite, 0600, &errors[i]);
				if (created[i])
				{
					std::array<uint8_t, 8> bytes {};
					bytes.fill(static_cast<uint8_t>(i + 1));
					uint32_t count = 0;
					file.file.Write(bytes.data(), bytes.size(), &count);
					if (count != bytes.size() || !HasBytes(file.file, bytes)) errors[i] = EIO;
				} else if (!file.file.IsInvalid()) errors[i] = EIO;
			});
		}
		while (ready.load() != workers) std::this_thread::yield();
		start.store(true);
		for (auto& thread: threads) thread.join();
		unsigned winners = 0;
		std::array<uint8_t, 8> expected {};
		for (unsigned i = 0; i < workers; ++i)
		{
			if (!Check(errors[i] == (created[i] ? 0 : EEXIST), "race result is success or EEXIST")) return false;
			if (created[i]) { ++winners; expected.fill(static_cast<uint8_t>(i + 1)); }
		}
		OpenFile reader;
		if (!Check(winners == 1 && reader.file.Open(Name(name), File::Mode::Read) && HasBytes(reader.file, expected),
		           "exactly one creator wins; losers preserve winner contents")) return false;
	}
	return true;
}
} // namespace

int main()
{
	Lumen::Core::core_file_init();
	std::error_code error;
	const auto directory = fs::temp_directory_path(error) / ("lumen-exclusive-create-" +
	    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	if (!Check(!error && fs::create_directory(directory, error) && !error, "private temporary directory")) return 1;
	const bool ok = Run(directory);
	fs::remove_all(directory, error);
	if (!Check(!error, "private temporary directory cleanup")) return 1;
	if (ok && !error) std::puts("CoreFileExclusiveCreateTest: PASS race=8x8 retained-handle/access/cursor/existing-bytes/errors");
	return ok && !error ? 0 : 1;
}
