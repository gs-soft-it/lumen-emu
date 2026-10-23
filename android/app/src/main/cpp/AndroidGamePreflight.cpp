// Lumen PS5 integration modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidGamePreflight.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t PREFLIGHT_ABI_VERSION = 1;
constexpr size_t PARAM_SFO_HEADER_SIZE = 20;
constexpr size_t PARAM_SFO_ENTRY_SIZE = 16;
constexpr size_t PARAM_SFO_MAX_SIZE = 1024 * 1024;
constexpr uint32_t PARAM_SFO_MAGIC = 0x46535000u;

struct RequiredFile
{
    const char* relativePath;
    bool isParamSfo;
};

constexpr RequiredFile REQUIRED_FILES[] = {
    {"eboot.bin", false},
    {"sce_sys/param.sfo", true},
    {"sce_module/libc.prx", false},
    {"sce_module/libSceFios2.prx", false},
};

uint16_t ReadLe16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool BuildPath(char* destination, size_t destinationSize,
               const char* root, const char* relativePath)
{
    const int written = std::snprintf(destination, destinationSize, "%s/%s",
                                      root, relativePath);
    return written > 0 && static_cast<size_t>(written) < destinationSize;
}

int OpenRegularFile(const char* path, uint64_t* fileSize)
{
    const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
    {
        return -1;
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size <= 0)
    {
        ::close(descriptor);
        return -1;
    }

    *fileSize = static_cast<uint64_t>(metadata.st_size);
    return descriptor;
}

bool ReadFully(int descriptor, uint8_t* destination, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t readSize =
            ::read(descriptor, destination + offset, size - offset);
        if (readSize < 0 && errno == EINTR)
        {
            continue;
        }
        if (readSize <= 0)
        {
            return false;
        }
        offset += static_cast<size_t>(readSize);
    }
    return true;
}

bool CopySfoString(const uint8_t* bytes, size_t size, uint32_t keyTableOffset,
                   uint32_t dataTableOffset, const uint8_t* entry,
                   const char* expectedKey, char* destination,
                   size_t destinationSize)
{
    const uint16_t keyOffset = ReadLe16(entry);
    const uint32_t valueLength = ReadLe32(entry + 4);
    const uint32_t valueOffset = ReadLe32(entry + 12);
    const uint64_t absoluteKeyOffset =
        static_cast<uint64_t>(keyTableOffset) + keyOffset;
    const uint64_t absoluteValueOffset =
        static_cast<uint64_t>(dataTableOffset) + valueOffset;
    if (absoluteKeyOffset >= size || absoluteValueOffset >= size ||
        valueLength == 0 || valueLength > size - absoluteValueOffset)
    {
        return false;
    }

    const auto* key = reinterpret_cast<const char*>(bytes + absoluteKeyOffset);
    const size_t remainingKeyBytes = size - static_cast<size_t>(absoluteKeyOffset);
    const void* keyTerminator = std::memchr(key, '\0', remainingKeyBytes);
    if (keyTerminator == nullptr || std::strcmp(key, expectedKey) != 0)
    {
        return false;
    }

    const auto* value = bytes + absoluteValueOffset;
    size_t copyLength = 0;
    while (copyLength < valueLength && value[copyLength] != 0)
    {
        ++copyLength;
    }
    if (copyLength == 0 || copyLength >= destinationSize)
    {
        return false;
    }
    std::memcpy(destination, value, copyLength);
    destination[copyLength] = '\0';
    return true;
}

bool ParseParamSfo(int descriptor, size_t size,
                   LumenAndroidGamePreflightStatusV1* status)
{
    if (size < PARAM_SFO_HEADER_SIZE || size > PARAM_SFO_MAX_SIZE)
    {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(std::malloc(size));
    if (bytes == nullptr)
    {
        return false;
    }
    const bool readOk = ReadFully(descriptor, bytes, size);
    if (!readOk || ReadLe32(bytes) != PARAM_SFO_MAGIC)
    {
        std::free(bytes);
        return false;
    }

    const uint32_t keyTableOffset = ReadLe32(bytes + 8);
    const uint32_t dataTableOffset = ReadLe32(bytes + 12);
    const uint32_t entryCount = ReadLe32(bytes + 16);
    const uint64_t entriesEnd = PARAM_SFO_HEADER_SIZE +
        static_cast<uint64_t>(entryCount) * PARAM_SFO_ENTRY_SIZE;
    if (entryCount == 0 || entriesEnd > size || keyTableOffset >= size ||
        dataTableOffset >= size)
    {
        std::free(bytes);
        return false;
    }

    bool titleIdFound = false;
    bool appVersionFound = false;
    bool contentIdFound = false;
    for (uint32_t index = 0; index < entryCount; ++index)
    {
        const uint8_t* entry = bytes + PARAM_SFO_HEADER_SIZE +
            static_cast<size_t>(index) * PARAM_SFO_ENTRY_SIZE;
        if (!titleIdFound)
        {
            titleIdFound = CopySfoString(
                bytes, size, keyTableOffset, dataTableOffset, entry, "TITLE_ID",
                status->titleId, sizeof(status->titleId));
        }
        if (!appVersionFound)
        {
            appVersionFound = CopySfoString(
                bytes, size, keyTableOffset, dataTableOffset, entry, "APP_VER",
                status->appVersion, sizeof(status->appVersion));
        }
        if (!contentIdFound)
        {
            contentIdFound = CopySfoString(
                bytes, size, keyTableOffset, dataTableOffset, entry, "CONTENT_ID",
                status->contentId, sizeof(status->contentId));
        }
    }

    std::free(bytes);
    return titleIdFound && appVersionFound && contentIdFound;
}

} // namespace

extern "C" uint32_t LumenAndroidGamePreflightV1(
    const char* gameDirectory, LumenAndroidGamePreflightStatusV1* status)
{
    if (gameDirectory == nullptr || gameDirectory[0] == '\0' || status == nullptr)
    {
        return LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_ARGUMENT_V1;
    }

    *status = {};
    status->abiVersion = PREFLIGHT_ABI_VERSION;
    char ps5MetadataPath[4096] {};
    if (!BuildPath(ps5MetadataPath, sizeof(ps5MetadataPath), gameDirectory,
                   "sce_sys/param.json")) {
        return LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_ARGUMENT_V1;
    }
    struct stat ps5Metadata {};
    if (::lstat(ps5MetadataPath, &ps5Metadata) == 0) {
        return LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1;
    }
    if (errno != ENOENT) {
        return LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_ARGUMENT_V1;
    }
    int paramSfoDescriptor = -1;
    size_t paramSfoSize = 0;

    for (const RequiredFile& requiredFile : REQUIRED_FILES)
    {
        char path[4096] {};
        if (!BuildPath(path, sizeof(path), gameDirectory,
                       requiredFile.relativePath))
        {
            if (paramSfoDescriptor >= 0)
            {
                ::close(paramSfoDescriptor);
            }
            return LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_ARGUMENT_V1;
        }

        uint64_t fileSize = 0;
        const int descriptor = OpenRegularFile(path, &fileSize);
        if (descriptor < 0)
        {
            if (paramSfoDescriptor >= 0)
            {
                ::close(paramSfoDescriptor);
            }
            return LUMEN_ANDROID_GAME_PREFLIGHT_REQUIRED_FILE_MISSING_V1;
        }
        ++status->requiredFileCount;
        status->requiredFileBytes += fileSize;

        if (requiredFile.isParamSfo)
        {
            paramSfoDescriptor = descriptor;
            paramSfoSize = static_cast<size_t>(fileSize);
        }
        else
        {
            ::close(descriptor);
        }
    }

    const bool paramSfoOk = paramSfoDescriptor >= 0 &&
        ParseParamSfo(paramSfoDescriptor, paramSfoSize, status);
    if (paramSfoDescriptor >= 0)
    {
        ::close(paramSfoDescriptor);
    }
    if (!paramSfoOk)
    {
        return LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_PARAM_SFO_V1;
    }
    return LUMEN_ANDROID_GAME_PREFLIGHT_OK_V1;
}
