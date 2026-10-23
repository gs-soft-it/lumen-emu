// Lumen game-launch cache preflight. SPDX-License-Identifier: GPL-2.0-only
#include "loader/gameCache.h"

#include "common/archive.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "GameTranslationCache.h"

// Reuse the SHA256 implementation already distributed with Lumen's SQLite.
#include "sqlite3.h"
extern "C" {
#include "sha2.c"
}

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace Loader::GameCache {
namespace {

std::string Finish(sha256_ctx* hash) {
	std::array<unsigned char, SHA256_DIGEST_SIZE> bytes {};
	sha256_final(hash, bytes.data());
	constexpr char hex[] = "0123456789abcdef";
	std::string result;
	for (const auto byte: bytes) {
		result += hex[byte >> 4];
		result += hex[byte & 15];
	}
	return result;
}

void Add(sha256_ctx* hash, std::string_view bytes) {
	sha256_update(hash, reinterpret_cast<const unsigned char*>(bytes.data()),
	              static_cast<unsigned int>(bytes.size()));
}

bool RegularPath(const std::filesystem::path& path, bool directory) {
	if (Common::IsArchivePath(path)) {
		const auto info = Common::File::GetInfo(path);
		return info && info->is_file != directory;
	}
	std::error_code error;
	const auto status = std::filesystem::symlink_status(path, error);
	if (error || std::filesystem::is_symlink(status) ||
	    (directory ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))) return false;
	return std::filesystem::canonical(path, error) == path && !error;
}

bool HashFile(const std::filesystem::path& path, std::string* digest) {
	if (!RegularPath(path, false)) return false;
	Common::File input(path, Common::File::Mode::Read);
	if (input.IsInvalid()) return false;
	const auto size = input.Size();
	sha256_ctx hash {};
	sha256_init(&hash);
	std::array<unsigned char, 64 * 1024> buffer {};
	std::uint64_t consumed = 0;
	while (consumed < size) {
		const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(size - consumed, buffer.size()));
		std::uint32_t read = 0;
		input.Read(buffer.data(), count, &read);
		if (read != count) return false;
		sha256_update(&hash, buffer.data(), read);
		consumed += read;
	}
	if (!RegularPath(path, false) || Common::File::Size(path) != size) return false;
	*digest = Finish(&hash);
	return true;
}

bool ContentId(const std::filesystem::path& root, const std::filesystem::path& executable,
               std::string* identity, std::string* error) {
	if (!Common::IsArchivePath(root)) {
		return Lumen::GuestCpu::GameTranslationCache::ExecutableContentId(root, executable, HashFile,
			[](std::string_view bytes) {
				sha256_ctx hash {};
				sha256_init(&hash);
				Add(&hash, bytes);
				return Finish(&hash);
			}, identity, error);
	}
	if (!RegularPath(root, true)) {
		*error = "game cache manifest root is missing, irregular, or linked";
		return false;
	}
	std::vector<std::filesystem::path> paths {executable};
	for (const auto& entry: Common::File::GetDirEntries(root)) {
		if (entry.name.ends_with(".prx")) paths.push_back(root / Common::PathFromUtf8(entry.name));
	}
	const auto module_root = root / "sce_module";
	std::vector<std::filesystem::path> directories;
	if (Common::File::IsDirectoryExisting(module_root) || Common::File::IsFileExisting(module_root) ||
	    (!Common::IsArchivePath(module_root) && std::filesystem::is_symlink(module_root))) directories.push_back(module_root);
	while (!directories.empty()) {
		const auto directory = std::move(directories.back());
		directories.pop_back();
		if (!RegularPath(directory, true)) {
			*error = "game cache executable module directory is irregular or linked";
			return false;
		}
		for (const auto& entry: Common::File::GetDirEntries(directory)) {
			const auto path = directory / Common::PathFromUtf8(entry.name);
			if (!RegularPath(path, !entry.is_file)) {
				*error = "game cache executable module path is irregular or linked";
				return false;
			}
			if (!entry.is_file) directories.push_back(path);
			else if (entry.name.ends_with(".prx")) paths.push_back(path);
		}
	}
	std::sort(paths.begin(), paths.end(), [&root](const auto& left, const auto& right) {
		return left.lexically_relative(root).generic_u8string() < right.lexically_relative(root).generic_u8string();
	});
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	sha256_ctx manifest {};
	sha256_init(&manifest);
	Add(&manifest, "lumen-fex-executable-manifest-v1\n");
	for (const auto& path: paths) {
		const auto relative = path.lexically_relative(root);
		if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
			*error = "game cache executable is outside its root";
			return false;
		}
		std::string digest;
		if (!HashFile(path, &digest)) {
			*error = "could not fingerprint a game cache executable module";
			return false;
		}
		const auto utf8 = relative.generic_u8string();
		Add(&manifest, std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
		Add(&manifest, std::string_view("\0", 1));
		Add(&manifest, digest);
		Add(&manifest, "\n");
	}
	*identity = Finish(&manifest);
	return true;
}

} // namespace

bool Prepare(const std::filesystem::path& input_root, const std::filesystem::path& input_executable,
             std::string* error) {
	bool enabled = false;
	if (!Lumen::GuestCpu::GameTranslationCache::Requested(&enabled, error)) return false;
	if (!enabled) return true;
	std::error_code path_error;
	const bool archive = Common::IsArchivePath(input_root);
	std::filesystem::path archive_host;
	std::uintmax_t archive_size = 0;
	std::filesystem::file_time_type archive_modified {};
	if (archive) {
		const auto source = Common::GetArchiveHostPath(input_root);
		const auto status = std::filesystem::symlink_status(source, path_error);
		if (path_error || !std::filesystem::is_regular_file(status)) {
			*error = "game cache archive source is irregular or linked";
			return false;
		}
		archive_host = std::filesystem::canonical(source, path_error);
		if (!path_error) archive_size = std::filesystem::file_size(archive_host, path_error);
		if (!path_error) archive_modified = std::filesystem::last_write_time(archive_host, path_error);
	}
	const auto root = archive ? Common::MakeArchivePath(archive_host) : std::filesystem::canonical(input_root, path_error);
	if (path_error) {
		*error = "could not resolve game cache manifest root";
		return false;
	}
	std::string identity;
	const char* explicit_identity = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID");
	if (explicit_identity) identity = explicit_identity;
	else if (!ContentId(root, root / input_executable.filename(), &identity, error)) return false;
	if (archive && (!std::filesystem::is_regular_file(std::filesystem::symlink_status(archive_host, path_error)) || path_error ||
	    std::filesystem::file_size(archive_host, path_error) != archive_size || path_error ||
	    std::filesystem::last_write_time(archive_host, path_error) != archive_modified || path_error)) {
		*error = "game cache archive changed during fingerprinting";
		return false;
	}
	return Lumen::GuestCpu::GameTranslationCache::PrepareEnvironment(identity, error);
}

} // namespace Loader::GameCache
