// SPDX-License-Identifier: GPL-2.0-only
#include "common/pathCaseCache.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if !defined(_WIN32)
namespace {

namespace fs = std::filesystem;
using Common::PathCaseCache;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "PathCaseCacheTests: failed: %s\n", message);
		std::abort();
	}
}

class TempDirectory {
public:
	explicit TempDirectory(const fs::path& parent) {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		path = parent / ("lumen-path-case-cache-" + std::to_string(unique));
		Check(fs::create_directories(path), "create fixture root");
	}
	~TempDirectory() {
		std::error_code error;
		fs::remove_all(path, error);
	}
	fs::path path;
};

void Write(const fs::path& path) {
	std::ofstream file(path);
	Check(static_cast<bool>(file << "fixture\n"), "write fixture file");
}

void ChangeStamp(const fs::path& directory) {
	// Keep invalidation deterministic on hosts with coarse timestamp precision.
	static int generation = 0;
	fs::last_write_time(directory, fs::file_time_type::clock::now() + std::chrono::hours(24) +
	                                   std::chrono::seconds(2 * ++generation));
}

void CheckHit(const PathCaseCache::LookupResult& result, const fs::path& expected) {
	if (!result.usable || !result.match || *result.match != expected) {
		const auto expected_text = expected.string();
		const auto match_text = result.match ? result.match->string() : std::string("<absent>");
		std::fprintf(stderr, "expected=%s usable=%d match=%s\n", expected_text.c_str(),
		             result.usable, match_text.c_str());
	}
	Check(result.usable && result.match && *result.match == expected, "resolve cached filename");
}

void CheckMiss(const PathCaseCache::LookupResult& result) {
	Check(result.usable && !result.match, "cache complete directory absence");
}

void TestRepeatedLookups(const fs::path& root) {
	const auto directory = root / "lookups";
	fs::create_directory(directory);
	Write(directory / "Actual.Bin");
	Write(directory / fs::path("asset-\xc3\xa9-\xe6\x97\xa5.bin"));
	fs::path unicode_path;
	for (const auto& entry: fs::directory_iterator(directory)) {
		if (entry.path().filename().string().starts_with("asset-")) {
			unicode_path = entry.path();
		}
	}
	Check(!unicode_path.empty(), "find native Unicode filename representation");
	PathCaseCache cache;
	CheckHit(cache.Lookup(directory, "ACTUAL.bin"), directory / "Actual.Bin");
	CheckMiss(cache.Lookup(directory, "absent.bin"));
	for (int i = 0; i < 100; ++i) {
		CheckHit(cache.Lookup(directory, "actual.BIN"), directory / "Actual.Bin");
		CheckMiss(cache.Lookup(directory, "absent.bin"));
	}
	auto unicode_query = unicode_path.filename().string();
	unicode_query.replace(0, 5, "ASSET");
	CheckHit(cache.Lookup(directory, unicode_query), unicode_path);
	Check(cache.ScanCount() == 1 && cache.CachedEntryCount() == 2,
	      "repeated hits and misses require one enumeration");
	cache.Clear();
	Check(cache.CachedDirectoryCount() == 0 && cache.CachedEntryCount() == 0,
	      "clear invalidates all directory indexes");
	CheckHit(cache.Lookup(directory, "actual.bin"), directory / "Actual.Bin");
	Check(cache.ScanCount() == 2, "lookup after explicit invalidation rebuilds index");
}

void TestMutations(const fs::path& root) {
	const auto directory = root / "mutations";
	fs::create_directory(directory);
	PathCaseCache cache;
	CheckMiss(cache.Lookup(directory, "new.bin"));
	Write(directory / "New.Bin");
	ChangeStamp(directory);
	CheckHit(cache.Lookup(directory, "new.bin"), directory / "New.Bin");
	fs::rename(directory / "New.Bin", directory / "Renamed.Bin");
	ChangeStamp(directory);
	CheckMiss(cache.Lookup(directory, "new.bin"));
	CheckHit(cache.Lookup(directory, "renamed.bin"), directory / "Renamed.Bin");
	fs::remove(directory / "Renamed.Bin");
	ChangeStamp(directory);
	CheckMiss(cache.Lookup(directory, "renamed.bin"));
	Check(cache.ScanCount() == 4, "create, rename and delete stamps invalidate indexes");

	const auto previous_stamp = fs::last_write_time(directory);
	fs::rename(directory, root / "old-directory");
	fs::create_directory(directory);
	Write(directory / "Replacement.Bin");
	fs::last_write_time(directory, previous_stamp);
	CheckHit(cache.Lookup(directory, "replacement.bin"), directory / "Replacement.Bin");
	Check(cache.ScanCount() == 5, "replacement directory inode invalidates matching mtime");
}

void TestCollisions(const fs::path& root) {
	const auto directory = root / "collisions";
	fs::create_directory(directory);
	Write(directory / "Mixed.Bin");
	if (fs::exists(directory / "MIXED.BIN")) {
		std::puts("PathCaseCacheTests: collision fixture SKIP (case-insensitive volume)");
		return;
	}
	Write(directory / "MIXED.BIN");
	Write(directory / "Unique.Bin");
	PathCaseCache cache;
	Check(!cache.Lookup(directory, "mixed.bin").usable,
	      "ambiguous casefold lookup requests legacy fallback");
	Check(!cache.Lookup(directory, "Mixed.Bin").usable,
	      "caller must resolve exact case before using index");
	CheckHit(cache.Lookup(directory, "unique.bin"), directory / "Unique.Bin");
	CheckMiss(cache.Lookup(directory, "absent.bin"));
	Check(cache.ScanCount() == 1, "collision does not disable unrelated directory names");
}

void TestErrors(const fs::path& root) {
	PathCaseCache cache;
	Check(!cache.Lookup(root / "missing", "name").usable,
	      "missing parent falls back without a negative index");
	Write(root / "regular-file");
	Check(!cache.Lookup(root / "regular-file", "name").usable,
	      "non-directory parent falls back");
	Check(cache.ScanCount() == 0 && cache.CachedDirectoryCount() == 0,
	      "invalid parent metadata cannot populate the cache");
#if !defined(_WIN32)
	const auto directory = root / "inaccessible";
	fs::create_directory(directory);
	Write(directory / "Hidden.Bin");
	fs::permissions(directory, fs::perms::none);
	const auto result = cache.Lookup(directory, "hidden.bin");
	fs::permissions(directory, fs::perms::owner_all);
	if (::geteuid() == 0) {
		std::puts("PathCaseCacheTests: permission fixture SKIP (privileged user)");
	} else {
		Check(!result.usable && cache.CachedDirectoryCount() == 0,
		      "failed directory scan is not cached");
		CheckHit(cache.Lookup(directory, "hidden.bin"), directory / "Hidden.Bin");
	}
#endif
}

void TestBudgets(const fs::path& root) {
	const auto first = root / "budget-first";
	const auto second = root / "budget-second";
	const auto oversized = root / "budget-oversized";
	fs::create_directory(first);
	fs::create_directory(second);
	fs::create_directory(oversized);
	for (const auto& directory: {first, second}) {
		Write(directory / "One");
		Write(directory / "Two");
	}
	for (const char* name: {"One", "Two", "Three", "Four"}) {
		Write(oversized / name);
	}
	PathCaseCache entries(2, 3);
	CheckMiss(entries.Lookup(first, "absent"));
	CheckMiss(entries.Lookup(second, "absent"));
	Check(entries.CachedDirectoryCount() == 1 && entries.CachedEntryCount() == 2,
	      "entry budget evicts old indexes");
	CheckMiss(entries.Lookup(second, "absent"));
	Check(entries.ScanCount() == 2, "retained index remains usable after budget eviction");
	Check(!entries.Lookup(oversized, "one").usable &&
	      !entries.Lookup(oversized, "absent").usable &&
	      entries.ScanCount() == 3 && entries.CachedEntryCount() == 2,
	      "oversized directories retain only an unusable sentinel without repeated scans");
	fs::remove(oversized / "Three");
	fs::remove(oversized / "Four");
	ChangeStamp(oversized);
	CheckHit(entries.Lookup(oversized, "one"), oversized / "One");
	CheckMiss(entries.Lookup(oversized, "absent"));
	Check(entries.ScanCount() == 4 && entries.CachedEntryCount() == 2,
	      "changed directory invalidates oversized sentinel and admits a complete index");
	PathCaseCache directories(1, 10);
	CheckMiss(directories.Lookup(first, "absent"));
	CheckMiss(directories.Lookup(second, "absent"));
	Check(directories.CachedDirectoryCount() == 1 && directories.ScanCount() == 2,
	      "directory count budget evicts old indexes");
	PathCaseCache disabled(0, 0);
	Check(!disabled.Lookup(first, "one").usable && disabled.ScanCount() == 0,
	      "zero budget performs no scans");
}

void TestSerializedThreads(const fs::path& root) {
	const auto directory = root / "threads";
	fs::create_directory(directory);
	Write(directory / "Shared.Bin");
	PathCaseCache cache;
	std::mutex owner_mutex;
	std::vector<std::thread> workers;
	for (int i = 0; i < 4; ++i) {
		workers.emplace_back([&] {
			for (int lookup = 0; lookup < 64; ++lookup) {
				std::lock_guard lock(owner_mutex);
				CheckHit(cache.Lookup(directory, "shared.bin"), directory / "Shared.Bin");
				CheckMiss(cache.Lookup(directory, "absent.bin"));
			}
		});
	}
	for (auto& worker: workers) {
		worker.join();
	}
	Check(cache.ScanCount() == 1, "owner lock serializes concurrent cache users");
}

} // namespace
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
	(void)argc;
	(void)argv;
	std::puts("PathCaseCacheTests: SKIP (POSIX-only path-case cache)");
	return 0;
#else
	TempDirectory temporary(argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path());
	TestRepeatedLookups(temporary.path);
	TestMutations(temporary.path);
	TestCollisions(temporary.path);
	TestErrors(temporary.path);
	TestBudgets(temporary.path);
	TestSerializedThreads(temporary.path);
	std::puts("PathCaseCacheTests: all cases passed");
	return 0;
#endif
}
