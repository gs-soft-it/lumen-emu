// Common validated game-launch cache policy. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace Lumen::GuestCpu::GameTranslationCache {

inline bool ValidContentIdentity(std::string_view identity) {
	if (identity.size() != 64) return false;
	return std::all_of(identity.begin(), identity.end(), [](char value) {
		return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
	});
}

inline bool PrepareDirectory(const std::string& directory) {
	if (directory.empty() || directory.front() != '/') return false;
	std::filesystem::path cursor;
	for (const auto& part: std::filesystem::path(directory)) {
		if (part == "." || part == "..") return false;
		cursor /= part;
		struct stat status {};
		if (lstat(cursor.c_str(), &status) != 0) {
			if (errno != ENOENT || mkdir(cursor.c_str(), 0700) != 0 || lstat(cursor.c_str(), &status) != 0) return false;
		}
		if (!S_ISDIR(status.st_mode)) return false;
	}
	struct stat status {};
	return lstat(directory.c_str(), &status) == 0 && status.st_uid == geteuid() && (status.st_mode & 0022) == 0;
}

inline bool Requested(bool* enabled, std::string* error) {
	const char* selector = std::getenv("LUMEN_FEX_TRANSLATION_CACHE");
	if (selector && std::string_view(selector) != "0" && std::string_view(selector) != "1") {
		*error = "FEX translation cache selector must be 0 or 1";
		return false;
	}
	*enabled = !selector || std::string_view(selector) == "1";
	return true;
}

// Hash callbacks keep this policy independent of the engine's crypto library.
// The serialized manifest is shared with Android, Qt and the Python launcher.
template <typename HashFile, typename HashBytes>
bool ExecutableContentId(const std::filesystem::path& root, const std::filesystem::path& executable,
                         HashFile hash_file, HashBytes hash_bytes, std::string* identity, std::string* error) {
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_directory(fs::symlink_status(root, ec)) || ec || fs::canonical(root, ec) != root || ec) {
		*error = "game cache manifest root is missing, irregular, or linked";
		return false;
	}
	std::vector<fs::path> paths {root / executable.filename()};
	fs::directory_iterator entry(root, ec), end;
	while (!ec && entry != end) {
		if (entry->path().extension() == ".prx") paths.push_back(entry->path());
		entry.increment(ec);
	}
	if (ec) {
		*error = "could not enumerate game cache executable modules";
		return false;
	}
	const auto module_root = root / "sce_module";
	const auto module_status = fs::symlink_status(module_root, ec);
	if (ec && ec != std::errc::no_such_file_or_directory) {
		*error = "could not inspect game cache executable module directory";
		return false;
	}
	if (!ec && fs::exists(module_status)) {
		if (fs::is_symlink(module_status) || !fs::is_directory(module_status)) {
			*error = "game cache executable module directory is irregular or linked";
			return false;
		}
		fs::recursive_directory_iterator module(module_root, ec), module_end;
		while (!ec && module != module_end) {
			const auto status = module->symlink_status(ec);
			if (ec || fs::is_symlink(status)) {
				*error = "game cache executable module path is linked or unreadable";
				return false;
			}
			if (module->path().extension() == ".prx" && !fs::is_directory(status)) paths.push_back(module->path());
			module.increment(ec);
		}
		if (ec) {
			*error = "could not enumerate game cache executable module directory";
			return false;
		}
	}
	std::sort(paths.begin(), paths.end(), [&root](const auto& left, const auto& right) {
		return left.lexically_relative(root).generic_u8string() < right.lexically_relative(root).generic_u8string();
	});
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	std::string manifest = "lumen-fex-executable-manifest-v1\n";
	for (const auto& path: paths) {
		ec.clear();
		const auto before = fs::symlink_status(path, ec);
		const auto relative = path.lexically_relative(root);
		if (ec || !fs::is_regular_file(before) || fs::canonical(path, ec) != path || ec ||
		    relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
			*error = "game cache executable module is irregular, linked, or outside its root";
			return false;
		}
		const auto size = fs::file_size(path, ec);
		const auto modified = fs::last_write_time(path, ec);
		std::string digest;
		if (ec || !hash_file(path, &digest) || !ValidContentIdentity(digest) ||
		    !fs::is_regular_file(fs::symlink_status(path, ec)) || ec || fs::canonical(path, ec) != path || ec ||
		    fs::file_size(path, ec) != size || ec || fs::last_write_time(path, ec) != modified || ec) {
			*error = "could not fingerprint an unchanged game cache executable module";
			return false;
		}
		const auto utf8 = relative.generic_u8string();
		manifest.append(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		manifest += '\0';
		manifest += digest;
		manifest += '\n';
	}
	*identity = hash_bytes(manifest);
	return ValidContentIdentity(*identity);
}

inline bool PrepareEnvironment(const std::string& validated_identity, std::string* error) {
	std::string identity = validated_identity;
	if (const char* value = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID")) identity = value;
	if (!ValidContentIdentity(identity)) {
		*error = "FEX translation cache content identity must be a SHA256";
		return false;
	}
	std::string directory;
	if (const char* value = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_DIR")) directory = value;
	else {
		const char* xdg = std::getenv("XDG_CACHE_HOME");
		const char* home = std::getenv("HOME");
		std::filesystem::path base;
		if (xdg) base = xdg;
		else if (home && home[0]) {
			base = home;
#if defined(__APPLE__)
			base /= "Library/Caches";
#else
			base /= ".cache";
#endif
		}
		if (!base.is_absolute()) {
			*error = "FEX translation cache requires an absolute user cache root";
			return false;
		}
		directory = (base / "LumenEmu" / "FexTranslations").string();
	}
	if (!PrepareDirectory(directory)) {
		*error = "FEX translation cache directory is not private, owned, and free of links";
		return false;
	}
	if (setenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID", identity.c_str(), 1) != 0 ||
	    setenv("LUMEN_FEX_TRANSLATION_CACHE_DIR", directory.c_str(), 1) != 0 ||
	    setenv("LUMEN_FEX_TRANSLATION_CACHE", "1", 1) != 0) {
		*error = "could not publish FEX game cache configuration";
		return false;
	}
	return true;
}

} // namespace Lumen::GuestCpu::GameTranslationCache
