// Lumen PS5 integration modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidGamePreflight.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void Put32(std::vector<unsigned char>& bytes, std::size_t offset, unsigned value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<unsigned char>(value >> (index * 8));
}

void Write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("fixture write failed");
}

std::vector<unsigned char> ParamSfo() {
    const std::array<std::string, 3> keys {"TITLE_ID", "APP_VER", "CONTENT_ID"};
    const std::array<std::string, 3> values {"TEST90004", "01.00", "UP0000-TEST90004_00-TESTCONTENT00000"};
    const unsigned key_table = 20 + 16 * keys.size();
    unsigned key_size = 0, value_size = 0;
    for (const auto& key : keys) key_size += key.size() + 1;
    for (const auto& value : values) value_size += value.size() + 1;
    const unsigned data_table = key_table + key_size;
    std::vector<unsigned char> bytes(data_table + value_size);
    Put32(bytes, 0, 0x46535000);
    Put32(bytes, 4, 0x101);
    Put32(bytes, 8, key_table);
    Put32(bytes, 12, data_table);
    Put32(bytes, 16, keys.size());
    unsigned key_offset = 0, value_offset = 0;
    for (unsigned index = 0; index < keys.size(); ++index) {
        const unsigned entry = 20 + index * 16;
        bytes[entry] = static_cast<unsigned char>(key_offset);
        bytes[entry + 2] = 4;
        bytes[entry + 3] = 2;
        Put32(bytes, entry + 4, values[index].size() + 1);
        Put32(bytes, entry + 8, values[index].size() + 1);
        Put32(bytes, entry + 12, value_offset);
        std::copy(keys[index].begin(), keys[index].end(), bytes.begin() + key_table + key_offset);
        std::copy(values[index].begin(), values[index].end(), bytes.begin() + data_table + value_offset);
        key_offset += keys[index].size() + 1;
        value_offset += values[index].size() + 1;
    }
    return bytes;
}

bool Check(const fs::path& root, unsigned expected, const char* scenario) {
    LumenAndroidGamePreflightStatusV1 status {};
    const auto actual = LumenAndroidGamePreflightV1(root.c_str(), &status);
    if (actual != expected || (expected == 4 && status.requiredFileCount != 0)) {
        std::fprintf(stderr, "%s: expected=%u actual=%u files=%u\n", scenario, expected, actual, status.requiredFileCount);
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::string temporary = (fs::temp_directory_path() / "lumen-ps5-admission-XXXXXX").string();
    if (::mkdtemp(temporary.data()) == nullptr) return 1;
    const fs::path root(temporary);
    bool passed = false;
    try {
        fs::create_directory(root / "sce_sys");
        fs::create_directory(root / "sce_module");
        Write(root / "eboot.bin", {1});
        Write(root / "sce_module/libc.prx", {1});
        Write(root / "sce_module/libSceFios2.prx", {1});
        Write(root / "sce_sys/param.sfo", ParamSfo());
        passed = Check(root, LUMEN_ANDROID_GAME_PREFLIGHT_OK_V1, "PS4 regression");
        Write(root / "sce_sys/param.json", {'{', '}'});
        passed &= Check(root, LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1, "dual metadata");
        fs::remove(root / "sce_sys/param.sfo");
        passed &= Check(root, LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1, "PS5 only");
        fs::remove(root / "sce_sys/param.json");
        fs::create_symlink("missing.json", root / "sce_sys/param.json");
        passed &= Check(root, LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1, "dangling PS5 marker");
    } catch (const std::exception& error) {
        passed = false;
        std::fprintf(stderr, "fixture failure: %s\n", error.what());
    }
    fs::remove_all(root);
    if (passed) std::puts("PS4 preflight regression and PS5 admission rejection passed");
    return passed ? 0 : 1;
}
