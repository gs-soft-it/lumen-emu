// SPDX-License-Identifier: GPL-2.0-only
#ifndef KYTY_COMMON_PATH_CASE_CACHE_H_
#define KYTY_COMMON_PATH_CASE_CACHE_H_

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace Common {

// The owner must serialize Lookup and Clear, including namespace mutations.
// MountPoints provides that lock. This is a directory-name index, not a cache
// of file contents or existence: callers must try the exact-case path first.
class PathCaseCache {
public:
	struct LookupResult {
		bool                                 usable = false;
		std::optional<std::filesystem::path> match;
	};

	explicit PathCaseCache(size_t max_directories = 128, size_t max_entries = 100000):
	    m_max_directories(max_directories), m_max_entries(max_entries) {}

	[[nodiscard]] LookupResult Lookup(const std::filesystem::path& parent,
	                                  const std::filesystem::path& component) {
		if (m_max_directories == 0 || m_max_entries == 0) {
			return {};
		}

		const auto before = ReadStamp(parent);
		auto cached = m_directories.find(parent);
		if (!before) {
			Erase(cached);
			return {};
		}
		if (cached != m_directories.end()) {
			if (cached->second.stamp == *before) {
				return Find(cached->second, component);
			}
			Erase(cached);
		}

		Directory directory;
		directory.stamp = *before;
		std::error_code error;
		++m_scan_count;
		std::filesystem::directory_iterator entry(parent, error), end;
		if (error) {
			return {};
		}
		for (; entry != end; entry.increment(error)) {
			if (error) {
				return {};
			}
			if (directory.entry_count == m_max_entries) {
				// Remember that this unchanged directory cannot fit. Discard the
				// partial index, so repeated lookups use legacy without rescanning.
				Directory oversized;
				oversized.stamp = *before;
				oversized.unusable = true;
				directory = std::move(oversized);
				break;
			}
			++directory.entry_count;
			const auto path = entry->path();
			auto [name, inserted] = directory.names.emplace(Fold(path.filename().string()), path);
			if (!inserted) {
				// Preserve the legacy iterator's choice for case-fold collisions.
				name->second.reset();
			}
		}
		if (error) {
			return {};
		}
		const auto after = ReadStamp(parent);
		if (!after || *after != *before) {
			return {};
		}

		if (m_directories.size() == m_max_directories ||
		    directory.entry_count > m_max_entries - m_entry_count) {
			Clear();
		}
		m_entry_count += directory.entry_count;
		auto inserted = m_directories.emplace(parent, std::move(directory)).first;
		return Find(inserted->second, component);
	}

	void Clear() {
		m_directories.clear();
		m_entry_count = 0;
	}

	[[nodiscard]] uint64_t ScanCount() const { return m_scan_count; }
	[[nodiscard]] size_t CachedDirectoryCount() const { return m_directories.size(); }
	[[nodiscard]] size_t CachedEntryCount() const { return m_entry_count; }

private:
	struct Stamp {
		uint64_t device;
		uint64_t inode;
		int64_t  mtime_seconds;
		int64_t  mtime_nanoseconds;
		int64_t  ctime_seconds;
		int64_t  ctime_nanoseconds;
		bool operator==(const Stamp&) const = default;
	};

	struct Directory {
		Stamp stamp {};
		size_t entry_count = 0;
		bool unusable = false;
		std::unordered_map<std::string, std::optional<std::filesystem::path>> names;
	};

	using Directories = std::unordered_map<std::filesystem::path, Directory>;

	static std::optional<Stamp> ReadStamp(const std::filesystem::path& path) {
#if defined(_WIN32)
		(void)path;
		return std::nullopt;
#else
		struct stat info {};
		if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
			return std::nullopt;
		}
#if defined(__APPLE__)
		const auto mtime = info.st_mtimespec;
		const auto ctime = info.st_ctimespec;
#else
		const auto mtime = info.st_mtim;
		const auto ctime = info.st_ctim;
#endif
		return Stamp {static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino),
		              static_cast<int64_t>(mtime.tv_sec), static_cast<int64_t>(mtime.tv_nsec),
		              static_cast<int64_t>(ctime.tv_sec), static_cast<int64_t>(ctime.tv_nsec)};
#endif
	}

	static std::string Fold(std::string value) {
		for (char& byte: value) {
			byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
		}
		return value;
	}

	static LookupResult Find(const Directory& directory, const std::filesystem::path& component) {
		if (directory.unusable) {
			return {};
		}
		const auto name = directory.names.find(Fold(component.string()));
		if (name == directory.names.end()) {
			return {true, std::nullopt};
		}
		if (!name->second) {
			return {};
		}
		return {true, name->second};
	}

	void Erase(Directories::iterator entry) {
		if (entry != m_directories.end()) {
			m_entry_count -= entry->second.entry_count;
			m_directories.erase(entry);
		}
	}

	size_t m_max_directories;
	size_t m_max_entries;
	size_t m_entry_count = 0;
	uint64_t m_scan_count = 0;
	Directories m_directories;
};

} // namespace Common

#endif // KYTY_COMMON_PATH_CASE_CACHE_H_
