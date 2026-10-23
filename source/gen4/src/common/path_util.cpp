// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "common/types.h"

#ifdef __APPLE__
#include <CoreFoundation/CFBundle.h>
#include <dlfcn.h>
#include <sys/param.h>
#endif

#ifndef MAX_PATH
#ifdef _WIN32
// This is the maximum number of UTF-16 code units permissible in Windows file paths
#define MAX_PATH 260
#include <Shlobj.h>
#include <windows.h>
#else
// This is the maximum number of UTF-8 code units permissible in all other OSes' file paths
#define MAX_PATH 1024
#endif
#endif

namespace Common::FS {

namespace fs = std::filesystem;

#ifdef __APPLE__
using IsTranslocatedURLFunc = Boolean (*)(CFURLRef path, bool* isTranslocated,
                                          CFErrorRef* __nullable error);
using CreateOriginalPathForURLFunc = CFURLRef __nullable (*)(CFURLRef translocatedPath,
                                                             CFErrorRef* __nullable error);

static CFURLRef UntranslocateBundlePath(const CFURLRef bundle_path) {
    if (void* security_handle =
            dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY)) {
        SCOPE_EXIT {
            dlclose(security_handle);
        };

        const auto IsTranslocatedURL = reinterpret_cast<IsTranslocatedURLFunc>(
            dlsym(security_handle, "SecTranslocateIsTranslocatedURL"));
        const auto CreateOriginalPathForURL = reinterpret_cast<CreateOriginalPathForURLFunc>(
            dlsym(security_handle, "SecTranslocateCreateOriginalPathForURL"));

        bool is_translocated = false;
        if (IsTranslocatedURL && CreateOriginalPathForURL &&
            IsTranslocatedURL(bundle_path, &is_translocated, nullptr) && is_translocated) {
            return CreateOriginalPathForURL(bundle_path, nullptr);
        }
    }
    return nullptr;
}

static std::optional<std::filesystem::path> GetBundleParentDirectory() {
    if (CFBundleRef bundle_ref = CFBundleGetMainBundle()) {
        if (CFURLRef bundle_url_ref = CFBundleCopyBundleURL(bundle_ref)) {
            SCOPE_EXIT {
                CFRelease(bundle_url_ref);
            };

            CFURLRef untranslocated_url_ref = UntranslocateBundlePath(bundle_url_ref);
            SCOPE_EXIT {
                if (untranslocated_url_ref) {
                    CFRelease(untranslocated_url_ref);
                }
            };

            char app_bundle_path[MAXPATHLEN];
            if (CFURLGetFileSystemRepresentation(
                    untranslocated_url_ref ? untranslocated_url_ref : bundle_url_ref, true,
                    reinterpret_cast<u8*>(app_bundle_path), sizeof(app_bundle_path))) {
                std::filesystem::path bundle_path{app_bundle_path};
                return bundle_path.parent_path();
            }
        }
    }
    return std::nullopt;
}
#endif

// Constructed explicitly by the session before logging or loading settings. A
// shared-library load must not touch the filesystem or another application's data.
static std::unordered_map<PathType, fs::path> UserPaths;

void InitializeUserPaths(const fs::path& user_dir) {
    const std::pair<PathType, const char*> children[] = {
        {PathType::LogDir, LOG_DIR}, {PathType::ScreenshotsDir, SCREENSHOTS_DIR},
        {PathType::ShaderDir, SHADER_DIR}, {PathType::GameDataDir, GAMEDATA_DIR},
        {PathType::TempDataDir, TEMPDATA_DIR}, {PathType::SysModuleDir, SYSMODULES_DIR},
        {PathType::DownloadDir, DOWNLOAD_DIR}, {PathType::CapturesDir, CAPTURES_DIR},
        {PathType::CheatsDir, CHEATS_DIR}, {PathType::PatchesDir, PATCHES_DIR},
        {PathType::MetaDataDir, METADATA_DIR}, {PathType::CustomTrophy, CUSTOM_TROPHY},
        {PathType::CustomConfigs, CUSTOM_CONFIGS}, {PathType::CacheDir, CACHE_DIR},
        {PathType::FontsDir, FONTS_DIR}, {PathType::HomeDir, HOME_DIR},
        {PathType::CustomModulesDir, CUSTOM_MODULES_DIR}
    };
    std::filesystem::create_directories(user_dir);
    UserPaths.insert_or_assign(PathType::UserDir, user_dir);
    for (const auto& [kind, child] : children) {
        const auto path = user_dir / child;
        std::filesystem::create_directories(path);
        UserPaths.insert_or_assign(kind, path);
    }
}

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty, path={}", PathToUTF8String(path));
        return false;
    }

#ifdef _WIN32
    if (path.u16string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#else
    if (path.u8string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#endif

    return true;
}

std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8_string = path.u8string();
    return std::string{u8_string.begin(), u8_string.end()};
}

const fs::path& GetUserPath(PathType shad_path) {
    return UserPaths.at(shad_path);
}

std::string GetUserPathString(PathType shad_path) {
    return PathToUTF8String(GetUserPath(shad_path));
}

void SetUserPath(PathType shad_path, const fs::path& new_path) {
    if (!std::filesystem::is_directory(new_path)) {
        LOG_ERROR(Common_Filesystem, "Filesystem object at new_path={} is not a directory",
                  PathToUTF8String(new_path));
        return;
    }

    UserPaths.insert_or_assign(shad_path, new_path);
}

std::optional<fs::path> FindGameByID(const fs::path& dir, const std::string& game_id,
                                     int max_depth) {
    if (max_depth < 0) {
        return std::nullopt;
    }

    // Check if this is the game we're looking for
    if (dir.filename() == game_id && fs::exists(dir / "sce_sys" / "param.sfo")) {
        auto eboot_path = dir / "eboot.bin";
        if (fs::exists(eboot_path)) {
            return eboot_path;
        }
    }

    // Recursively search subdirectories
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (auto found = FindGameByID(entry.path(), game_id, max_depth - 1)) {
            return found;
        }
    }

    return std::nullopt;
}

} // namespace Common::FS
