// Synthetic game-launch cache policy tests. SPDX-License-Identifier: GPL-2.0-only
#include "GameTranslationCache.h"
#include "sqlite3.h"
#include "sha2.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>

namespace fs = std::filesystem;
namespace Cache = Lumen::GuestCpu::GameTranslationCache;

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
	if (!condition) { std::fprintf(stderr, "game cache test: %s\n", name); ++failures; }
}

struct EnvironmentGuard {
	const char* key;
	std::optional<std::string> original;
	explicit EnvironmentGuard(const char* name): key(name) {
		if (const char* value = std::getenv(key)) original = value;
		unsetenv(key);
	}
	~EnvironmentGuard() {
		if (original) setenv(key, original->c_str(), 1);
		else unsetenv(key);
	}
};

std::string Finish(sha256_ctx* hash) {
	std::array<unsigned char, SHA256_DIGEST_SIZE> bytes {};
	sha256_final(hash, bytes.data());
	std::string output;
	constexpr char hex[] = "0123456789abcdef";
	for (const auto byte: bytes) { output += hex[byte >> 4]; output += hex[byte & 15]; }
	return output;
}

std::string HashBytes(std::string_view bytes) {
	sha256_ctx hash {};
	sha256_init(&hash);
	sha256_update(&hash, reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<unsigned int>(bytes.size()));
	return Finish(&hash);
}

bool HashFile(const fs::path& path, std::string* digest) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) return false;
	sha256_ctx hash {};
	sha256_init(&hash);
	std::array<unsigned char, 8192> buffer {};
	while (stream) {
		stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
		sha256_update(&hash, buffer.data(), static_cast<unsigned int>(stream.gcount()));
	}
	if (!stream.eof()) return false;
	*digest = Finish(&hash);
	return true;
}

void Write(const fs::path& path, std::string_view bytes) {
	fs::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary);
	stream.write(bytes.data(), bytes.size());
	Check(stream.good(), "fixture write");
}
} // namespace

int main() {
	EnvironmentGuard selector("LUMEN_FEX_TRANSLATION_CACHE");
	EnvironmentGuard identity_env("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID");
	EnvironmentGuard directory_env("LUMEN_FEX_TRANSLATION_CACHE_DIR");
	EnvironmentGuard xdg("XDG_CACHE_HOME");
	std::string pattern = (fs::canonical(fs::temp_directory_path()) / "lumen-game-cache-XXXXXX").string();
	std::vector<char> temporary(pattern.begin(), pattern.end());
	temporary.push_back('\0');
	const char* created = mkdtemp(temporary.data());
	if (!created) return 1;
	const fs::path root(created);
	const fs::path game = root / "game";
	Write(game / "eboot.bin", "guest\n");
	Write(game / "liba.prx", "root\n");
	Write(game / "sce_module/nested/z.prx", "nested\n");
	Write(game / "sce_module/readme.txt", "ignored\n");
	std::string error, identity;
	Check(Cache::ExecutableContentId(game, "eboot.bin", HashFile, HashBytes, &identity, &error), "default manifest");
	Check(identity == "81adb1901721301bf9695e5b0eeb52af06e42867096575e0abcdf66be5a889f5", "cross-platform SHA256 vector");
	const auto original = identity;
	Write(game / "sce_module/readme.txt", "changed ignored bytes\n");
	Check(Cache::ExecutableContentId(game, "eboot.bin", HashFile, HashBytes, &identity, &error) && identity == original,
	      "non-executable bytes excluded");
	Write(game / "sce_module/nested/z.prx", "changed nested bytes\n");
	Check(Cache::ExecutableContentId(game, "eboot.bin", HashFile, HashBytes, &identity, &error) && identity != original,
	      "nested executable content invalidates identity");
	bool enabled = false;
	Check(Cache::Requested(&enabled, &error) && enabled, "game cache default on");
	setenv("XDG_CACHE_HOME", (root / "user-cache").c_str(), 1);
	Check(Cache::PrepareEnvironment(identity, &error), "automatic private directory and identity");
	const auto directory = std::string(std::getenv("LUMEN_FEX_TRANSLATION_CACHE_DIR"));
	struct stat mode {};
	Check(lstat(directory.c_str(), &mode) == 0 && (mode.st_mode & 0077) == 0, "automatic cache directory mode 0700");
	Check(std::string(std::getenv("LUMEN_FEX_TRANSLATION_CACHE")) == "1", "configuration published before admission");
	const std::string uppercase(64, 'A');
	setenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID", uppercase.c_str(), 1);
	Check(Cache::PrepareEnvironment({}, &error), "uppercase explicit SHA256 remains valid");
	Check(std::string(std::getenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID")) == uppercase, "explicit identity preserved");
	setenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID", "invalid", 1);
	Check(!Cache::PrepareEnvironment(identity, &error), "malformed explicit identity rejected");
	setenv("LUMEN_FEX_TRANSLATION_CACHE", "0", 1);
	Check(Cache::Requested(&enabled, &error) && !enabled, "explicit zero disables game cache");
	setenv("LUMEN_FEX_TRANSLATION_CACHE", "true", 1);
	Check(!Cache::Requested(&enabled, &error), "malformed selector rejected");
	unsetenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID");
	fs::create_directory_symlink(directory, root / "linked-cache");
	setenv("LUMEN_FEX_TRANSLATION_CACHE_DIR", (root / "linked-cache").c_str(), 1);
	Check(!Cache::PrepareEnvironment(identity, &error), "linked cache directory rejected");
	fs::remove(game / "liba.prx");
	fs::create_symlink(game / "sce_module/nested/z.prx", game / "liba.prx");
	Check(!Cache::ExecutableContentId(game, "eboot.bin", HashFile, HashBytes, &identity, &error), "linked module rejected");
	fs::remove(game / "liba.prx");
	auto changing_hash = [](const fs::path& path, std::string* digest) {
		if (!HashFile(path, digest)) return false;
		std::ofstream stream(path, std::ios::binary | std::ios::app);
		stream << "changed";
		return true;
	};
	Check(!Cache::ExecutableContentId(game, "eboot.bin", changing_hash, HashBytes, &identity, &error), "changing module rejected");
	fs::remove_all(root);
	if (failures) return 1;
	std::puts("game translation cache tests passed (synthetic launch policy only)");
	return 0;
}
