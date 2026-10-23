// Lumen local adapter storage. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <xxhash.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace Lumen::FexCore::TranslationCache {

// The pinned incremental cache format is FOZ v6 / FEX blob v3. These bounds
// apply to our optional local snapshots, not to the upstream cache format.
inline constexpr std::size_t MaxDataBytes = 128 * 1024 * 1024;
inline constexpr std::size_t MaxIndexBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaxBlobBytes = 32 * 1024 * 1024;
inline constexpr std::array<unsigned char, 16> Magic {
    0x81, 'F', 'O', 'S', 'S', 'I', 'L', 'I', 'Z', 'E', 'D', 'B', 0, 0, 0, 6};

// Both modes are separated from each other and from older identities that
// predate the renderer contract. This must precede Session::Start/native load.
inline void AppendMemoryHandoffDomain(std::string& identity, bool intervalHandoff) {
    identity.append("\0lumen-memory-coherence-v2-progress-v1:", sizeof("\0lumen-memory-coherence-v2-progress-v1:") - 1);
    identity.push_back(intervalHandoff ? '1' : '0');
}

inline std::string Digest(std::span<const unsigned char> bytes) {
    const auto hash = XXH3_128bits(bytes.data(), bytes.size());
    char output[33];
    std::snprintf(output, sizeof(output), "%016llx%016llx",
                  static_cast<unsigned long long>(hash.high64),
                  static_cast<unsigned long long>(hash.low64));
    return output;
}

inline std::string Digest(std::string_view bytes) {
    return Digest(std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

inline bool Hex(std::string_view text, std::size_t size) {
    if (text.size() != size) return false;
    for (const char value : text) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

template <typename T> inline T Read(const unsigned char* bytes) {
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// Validate the exact bounded subset the pinned FEX writer produces before
// allowing its loader to interpret host code, entrypoints or relocations.
inline bool ValidateBlob(std::span<const unsigned char> bytes) {
    if (bytes.size() < 40 || bytes.size() > MaxBlobBytes) return false;
    const auto guestSize = Read<std::uint32_t>(bytes.data());
    const auto hostSize = Read<std::uint32_t>(bytes.data() + 4);
    const auto entries = Read<std::uint32_t>(bytes.data() + 8);
    const auto small = Read<std::uint32_t>(bytes.data() + 12);
    const auto thunks = Read<std::uint32_t>(bytes.data() + 16);
    const auto pages = Read<std::uint32_t>(bytes.data() + 20);
    if (guestSize == 0 || guestSize > 16 * 1024 * 1024 || hostSize == 0 ||
        hostSize > 8 * 1024 * 1024 || hostSize % 16 != 0 || entries == 0 ||
        entries > 5000 || small > 65536 || thunks != 0 || pages == 0 || pages > 4096) return false;
    // Thunk relocations are outside Lumen's registered Custom-IR HLE bridge.
    // Denying them also prevents the absent upstream ThunkHandler being used.
    const std::uint64_t total = 40ULL + hostSize + 8ULL * pages + 12ULL * entries +
                                14ULL * small + 37ULL * thunks + guestSize;
    if (total != bytes.size()) return false;
    const auto liveHash = XXH3_128bits(bytes.data() + bytes.size() - guestSize, guestSize);
    if (std::memcmp(bytes.data() + 24, &liveHash, sizeof(liveHash)) != 0) return false;
    const auto* host = bytes.data() + 40;
    const auto tail = Read<std::uint32_t>(host);
    if (tail < 8 || tail % 8 != 0 || tail > hostSize || hostSize - tail < 48 ||
        Read<std::uint64_t>(host + tail) != hostSize || Read<std::uint64_t>(host + tail + 16) != guestSize ||
        Read<std::uint32_t>(host + tail + 32) != 0 || host[tail + 36] > 1) return false;
    const auto ripCount = Read<std::uint32_t>(host + tail + 24);
    const auto ripOffset = Read<std::uint32_t>(host + tail + 28);
    if (ripCount == 0 || ripCount > 5000 || ripOffset != 48) return false;
    const auto memoryThunkOffset = Read<std::uint32_t>(host + tail + 40);
    const auto memoryThunkSize = Read<std::uint32_t>(host + tail + 44);
    if (memoryThunkSize == 0) {
        if (memoryThunkOffset != 0) return false;
    } else if (memoryThunkOffset < 4 || memoryThunkOffset % 4 != 0 || memoryThunkSize % 4 != 0 ||
        memoryThunkOffset >= tail || memoryThunkSize > tail - memoryThunkOffset) return false;
    const auto inMemoryThunk = [=](std::uint64_t offset) {
        return memoryThunkSize != 0 && offset >= memoryThunkOffset && offset - memoryThunkOffset < memoryThunkSize;
    };
    std::size_t ripCursor = tail + ripOffset;
    std::int64_t hostCursor = 0, guestCursor = 0;
    std::vector<std::uint64_t> instructionOffsets;
    for (std::uint32_t i = 0; i < ripCount; ++i) {
        if (ripCursor >= hostSize) return false;
        const auto tag = host[ripCursor];
        std::int64_t hostDelta {}, guestDelta {};
        std::size_t encodedSize {};
        if ((tag & 0x80) == 0) {
            encodedSize = 1; hostDelta = ((tag & 15) + 1) * 4; guestDelta = ((tag >> 4) & 7) + 1;
        } else if ((tag >> 6) == 2) {
            encodedSize = 2;
            if (encodedSize > hostSize - ripCursor) return false;
            hostDelta = static_cast<std::int8_t>(host[ripCursor + 1]) * 4;
            guestDelta = (tag & 63) >= 32 ? static_cast<int>(tag & 63) - 64 : tag & 63;
        } else if (tag == 0xc0) {
            encodedSize = 9;
            if (encodedSize > hostSize - ripCursor) return false;
            hostDelta = Read<std::int32_t>(host + ripCursor + 1);
            guestDelta = Read<std::int32_t>(host + ripCursor + 5);
        } else if (tag == 0xe0) {
            encodedSize = 17;
            if (encodedSize > hostSize - ripCursor) return false;
            hostDelta = Read<std::int64_t>(host + ripCursor + 1);
            guestDelta = Read<std::int64_t>(host + ripCursor + 9);
        } else return false;
        if (hostDelta < -hostCursor || hostDelta >= static_cast<std::int64_t>(tail) - hostCursor ||
            guestDelta < -guestCursor || guestDelta >= static_cast<std::int64_t>(guestSize) - guestCursor) return false;
        hostCursor += hostDelta;
        guestCursor += guestDelta;
        if (hostCursor < 4 || hostCursor % 4 != 0 || inMemoryThunk(hostCursor)) return false;
        instructionOffsets.push_back(static_cast<std::uint64_t>(guestCursor));
        ripCursor += encodedSize;
    }
    if (hostSize - ripCursor >= 16) return false; // only final alignment padding
    std::size_t cursor = 40 + hostSize;
    std::uint64_t pagePhase = UINT64_MAX;
    bool rootPage = false;
    std::vector<std::int64_t> pageOffsets;
    for (std::uint32_t index = 0; index < pages; ++index, cursor += 8) {
        const auto offset = Read<std::int64_t>(bytes.data() + cursor);
        // The pinned decoder records the page at OpAddress+MAX_INST_SIZE(15),
        // even when the actual final instruction ends before that page. Only
        // this conservative lookahead may extend past the hashed guest span;
        // instruction markers and executable entrypoints remain inside it.
        if (offset <= -4096 || offset >= static_cast<std::int64_t>(guestSize) + 15) return false;
        const auto phase = static_cast<std::uint64_t>(offset) & 4095;
        if (pagePhase != UINT64_MAX && phase != pagePhase) return false;
        pagePhase = phase;
        rootPage |= offset <= 0;
        pageOffsets.push_back(offset);
    }
    std::sort(pageOffsets.begin(), pageOffsets.end());
    if (!rootPage || std::adjacent_find(pageOffsets.begin(), pageOffsets.end()) != pageOffsets.end()) return false;
    if (pagePhase != ((0 - Read<std::uint64_t>(host + tail + 8)) & 4095)) return false;
    const auto rootLow = (4096 - pagePhase) & 4095;
    instructionOffsets.push_back(guestSize - 1);
    for (const auto offset : instructionOffsets) {
        const auto coveringPage = static_cast<std::int64_t>(((rootLow + offset) / 4096) * 4096) -
                                  static_cast<std::int64_t>(rootLow);
        if (!std::binary_search(pageOffsets.begin(), pageOffsets.end(), coveringPage)) return false;
    }
    bool rootEntry = false;
    std::vector<std::uint64_t> entryOffsets;
    for (std::uint32_t index = 0; index < entries; ++index, cursor += 8) {
        const auto offset = Read<std::uint64_t>(bytes.data() + cursor);
        if (offset >= guestSize) return false;
        const auto coveringPage = static_cast<std::int64_t>(((rootLow + offset) / 4096) * 4096) -
                                  static_cast<std::int64_t>(rootLow);
        if (!std::binary_search(pageOffsets.begin(), pageOffsets.end(), coveringPage)) return false;
        rootEntry |= offset == 0;
        entryOffsets.push_back(offset);
    }
    std::sort(entryOffsets.begin(), entryOffsets.end());
    if (!rootEntry || std::adjacent_find(entryOffsets.begin(), entryOffsets.end()) != entryOffsets.end()) return false;
    for (std::uint32_t index = 0; index < entries; ++index, cursor += 4) {
        const auto offset = Read<std::uint32_t>(bytes.data() + cursor);
        if (offset < 4 || offset >= tail || offset % 4 != 0 || inMemoryThunk(offset)) return false;
    }
    bool tailRipRelocated = false;
    for (std::uint32_t index = 0; index < small; ++index, cursor += 14) {
        const auto offset = Read<std::uint32_t>(bytes.data() + cursor);
        const auto type = bytes[cursor + 4];
        const std::uint32_t width = type == 3 ? 16 : 8;
        if ((type != 0 && type != 2 && type != 3) || offset % 4 != 0 ||
            offset > hostSize || width > hostSize - offset) return false;
        if (type == 2 && offset == tail + 8 && Read<std::uint64_t>(bytes.data() + cursor + 5) == 0) {
            if (tailRipRelocated) return false;
            tailRipRelocated = true;
        } else if (offset < 4 || offset >= tail || width > tail - offset) return false;
        if (type == 0) {
            // The pinned backend names exitlinker(0). The instrumented FEX
            // overlays name guard table(1), ordinary callback(2), REP bulk(3),
            // preserving callback(4), read-guard directory(5), progress flag(6)
            // and retirement callback(7). FEX binds
            // these exact process-local providers before executing a hit.
#if (defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED) && \
     LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED == 1) || \
    (defined(__ANDROID__) && defined(LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED) && \
     LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED == 1)
            constexpr std::uint32_t lastNamedSymbol = 7;
#else
            constexpr std::uint32_t lastNamedSymbol = 0;
#endif
            if (Read<std::uint32_t>(bytes.data() + cursor + 5) > lastNamedSymbol) return false;
        }
        if (type == 3 && bytes[cursor + 5] > 30) return false;
    }
    return tailRipRelocated;
}

struct Record {
    std::array<unsigned char, 40> key;
    std::size_t offset;
    std::size_t size;
    bool metadata;
};

inline bool Records(std::span<const unsigned char> bytes, std::vector<Record>& records) {
    if (bytes.size() < Magic.size() || !std::equal(Magic.begin(), Magic.end(), bytes.begin())) return false;
    std::size_t cursor = Magic.size();
    while (cursor < bytes.size()) {
        if (records.size() >= 200000 || bytes.size() - cursor < 56) return false;
        Record record {};
        std::memcpy(record.key.data(), bytes.data() + cursor, record.key.size());
        record.metadata = std::all_of(record.key.begin(), record.key.end(), [](auto b) { return b == 0xff; });
        if (!record.metadata) {
            if (!Hex(std::string_view(reinterpret_cast<const char*>(record.key.data()), 16), 16) ||
                !std::all_of(record.key.begin() + 16, record.key.end(), [](auto b) { return b == 0; })) return false;
        }
        const auto* header = bytes.data() + cursor + 40;
        record.size = Read<std::uint32_t>(header);
        if (Read<std::uint32_t>(header + 4) != 1 || Read<std::uint32_t>(header + 8) != 0 ||
            Read<std::uint32_t>(header + 12) != record.size) return false;
        record.offset = cursor + 56;
        if (record.size > bytes.size() - record.offset) return false;
        records.push_back(record);
        cursor = record.offset + record.size;
    }
    return true;
}

inline bool ValidateDatabase(std::span<const unsigned char> data, std::span<const unsigned char> index) {
    std::vector<Record> dataRecords, indexRecords;
    if (data.size() > MaxDataBytes || index.size() > MaxIndexBytes ||
        !Records(data, dataRecords) || !Records(index, indexRecords)) return false;
    std::map<std::uint64_t, const Record*> offsets;
    for (const auto& record : dataRecords) {
        if (record.metadata ? (record.size < 11 || record.size > 65536) :
                              !ValidateBlob(data.subspan(record.offset, record.size))) return false;
        offsets.emplace(record.offset, &record);
    }
    bool metadata = false;
    for (const auto& record : indexRecords) {
        if (record.size != 28) return false;
        const auto* payload = index.data() + record.offset;
        const auto size = Read<std::uint32_t>(payload + 8);
        const auto offset = Read<std::uint64_t>(payload + 20);
        const auto found = offsets.find(offset);
        if (found == offsets.end() || found->second->size != size || found->second->key != record.key) return false;
        std::uint64_t key = UINT64_MAX;
        if (!record.metadata) {
            const auto* begin = reinterpret_cast<const char*>(record.key.data());
            if (std::from_chars(begin, begin + 16, key, 16).ec != std::errc {}) return false;
        } else {
            metadata = true;
        }
        if (Read<std::uint64_t>(payload) != key) return false;
        offsets.erase(found);
    }
    return metadata && offsets.empty();
}

inline bool ReadFile(const std::string& path, std::vector<unsigned char>& bytes, std::size_t limit) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct CloseOnExit { int descriptor; ~CloseOnExit() { close(descriptor); } } guard {fd};
    struct stat st {};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              st.st_nlink == 1 && st.st_size >= 0 && static_cast<std::uint64_t>(st.st_size) <= limit;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(st.st_size));
        std::size_t cursor = 0;
        while (cursor < bytes.size()) {
            const auto count = read(fd, bytes.data() + cursor, bytes.size() - cursor);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            cursor += static_cast<std::size_t>(count);
        }
        unsigned char tail;
        if (ok && read(fd, &tail, 1) != 0) ok = false;
    }
    return ok;
}

inline bool WriteFile(const std::string& path, std::span<const unsigned char> bytes) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    bool ok = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto count = write(fd, bytes.data() + cursor, bytes.size() - cursor);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        cursor += static_cast<std::size_t>(count);
    }
    if (ok) ok = fsync(fd) == 0;
    close(fd);
    if (!ok) unlink(path.c_str());
    return ok;
}

inline bool WriteFile(const std::string& path, std::string_view text) {
    return WriteFile(path, std::span(reinterpret_cast<const unsigned char*>(text.data()), text.size()));
}

inline bool PrepareDirectory(const std::string& path) {
    const auto denied = [](const char* stage, int error) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_TRANSLATION_CACHE_STORAGE_V1 stage=%s errno=%d", stage, error);
#else
        std::fprintf(stderr, "LUMEN_FEX_TRANSLATION_CACHE_STORAGE_V1 stage=%s errno=%d\n", stage, error);
#endif
        errno = error;
        return false;
    };
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos)
        return denied("path", EINVAL);
    std::filesystem::path cursor;
    for (const auto& part : std::filesystem::path(path)) {
        if (part == "." || part == "..") return denied("component", EINVAL);
        cursor /= part;
        struct stat st {};
        if (lstat(cursor.c_str(), &st) != 0) {
            if (errno != ENOENT) return denied("stat-component", errno);
            if (mkdir(cursor.c_str(), 0700) != 0) return denied("mkdir-component", errno);
            if (lstat(cursor.c_str(), &st) != 0) return denied("stat-created-component", errno);
        }
        if (!S_ISDIR(st.st_mode)) return denied("directory-component", ENOTDIR);
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return denied("stat-leaf", errno);
    if (st.st_uid != geteuid() || (st.st_mode & 0022) != 0)
        return denied("leaf-owner-mode", EACCES);
    return true;
}

class PublishLock {
public:
    explicit PublishLock(const std::string& path) {
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat st {};
        if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                        st.st_nlink != 1 || flock(fd, LOCK_EX | LOCK_NB) != 0)) { close(fd); fd = -1; }
    }
    ~PublishLock() { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
    bool Valid() const { return fd >= 0; }
private:
    int fd = -1;
};

// FEX's pinned FOZ writer uses flock on data then index while appending both
// records. Separate open descriptions conflict with those locks on Darwin
// and Android too, so this captures only completed pairs without a guest stop.
class CacheReadLock {
public:
    explicit CacheReadLock(const std::string& path, std::size_t limit) {
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st {};
        if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                        st.st_nlink != 1 || st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > limit ||
                        flock(fd, LOCK_EX | LOCK_NB) != 0)) { close(fd); fd = -1; }
        if (fd >= 0 && (fstat(fd, &st) != 0 || st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > limit)) {
            flock(fd, LOCK_UN); close(fd); fd = -1;
        }
        if (fd >= 0) size = static_cast<std::size_t>(st.st_size);
    }
    ~CacheReadLock() { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
    bool Valid() const { return fd >= 0; }
    std::size_t Size() const { return size; }
private:
    int fd = -1;
    std::size_t size = 0;
};

class StagingDirectory {
public:
    ~StagingDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    std::string path;
};

class Session {
public:
    ~Session() {
        StopCheckpointing();
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    bool Start(const std::string& directory, const std::string& bucket) {
        if (!Hex(bucket, 32) || !PrepareDirectory(directory)) return false;
        root = directory + "/" + bucket;
        if (!PrepareDirectory(root)) return false;
        PublishLock lock(root + "/publish.lock");
        if (!lock.Valid()) return false;
        std::string pattern = root + "/session-XXXXXX";
        if (mkdtemp(pattern.data()) == nullptr) return false;
        path = pattern;
        std::vector<unsigned char> current;
        if (ReadFile(root + "/current", current, 80)) {
            const std::string name(current.begin(), current.end());
            if (name.starts_with("snapshot-") && name.size() < 64 &&
                std::all_of(name.begin(), name.end(), [](auto c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-';
                })) {
                std::vector<unsigned char> data, index;
                if (ReadSnapshot(root + "/" + name, data, index)) {
                    loaded = WriteFile(path + "/RWCacheDB.foz", data) &&
                             WriteFile(path + "/RWCacheDB_idx.foz", index);
                    if (!loaded) return false;
                }
            }
        }
        return true;
    }

    // A final checkpoint after the context closes also captures its drained
    // queue. Live checkpoints capture completed records only; pending native
    // writer jobs are deliberately excluded.
    bool Seal() { return Checkpoint(true); }

    bool Checkpoint(bool force = false) {
        std::lock_guard checkpointLock(checkpointMutex);
        if (path.empty()) return false;
        std::vector<unsigned char> data, index;
        {
            CacheReadLock dataLock(path + "/RWCacheDB.foz", MaxDataBytes);
            if (!dataLock.Valid()) return false;
            CacheReadLock indexLock(path + "/RWCacheDB_idx.foz", MaxIndexBytes);
            if (!indexLock.Valid()) return false;
            if (!force && dataLock.Size() == publishedDataSize && indexLock.Size() == publishedIndexSize) return true;
            if (!ReadFile(path + "/RWCacheDB.foz", data, MaxDataBytes) ||
                !ReadFile(path + "/RWCacheDB_idx.foz", index, MaxIndexBytes)) return false;
        }
        // The writer can append again as soon as the bounded byte copies are
        // complete. Validation, hashing and durable publication hold no FEX lock.
        if (!ValidateDatabase(data, index)) return false;
        StagingDirectory staging;
        staging.path = root + "/checkpoint-XXXXXX";
        if (mkdtemp(staging.path.data()) == nullptr) { staging.path.clear(); return false; }
        if (!WriteFile(staging.path + "/RWCacheDB.foz", data) ||
            !WriteFile(staging.path + "/RWCacheDB_idx.foz", index)) return false;
        std::ostringstream manifest;
        manifest << "LUMEN_FEX_CACHE_V1\n" << Digest(data) << ' ' << data.size() << '\n'
                 << Digest(index) << ' ' << index.size() << '\n';
        if (!WriteFile(staging.path + "/seal", manifest.str())) return false;
        PublishLock lock(root + "/publish.lock");
        if (!lock.Valid()) return false;
        std::vector<unsigned char> previousPointer;
        ReadFile(root + "/current", previousPointer, 80);
        const std::string previous(previousPointer.begin(), previousPointer.end());
        const auto name = "snapshot-" + std::filesystem::path(staging.path).filename().string().substr(11);
        const std::string published = root + "/" + name;
        if (rename(staging.path.c_str(), published.c_str()) != 0) return false;
        staging.path = published;
        const std::string pointer = root + "/current-" + name;
        if (!WriteFile(pointer, name)) return false;
        if (rename(pointer.c_str(), (root + "/current").c_str()) != 0) { unlink(pointer.c_str()); return false; }
        staging.path.clear();
        publishedDataSize = data.size();
        publishedIndexSize = index.size();
        // All consumers clone snapshots while holding this same lock. Once
        // current is published, retain it and one rollback snapshot only.
        std::error_code error;
        for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            const auto candidate = it->path().filename().string();
            if (candidate.starts_with("snapshot-") && candidate != name && candidate != previous &&
                candidate.size() < 64 && it->is_directory(error) && !it->is_symlink(error)) {
                std::filesystem::remove_all(it->path(), error);
            }
        }
        return true;
    }

    bool StartCheckpointing() noexcept {
        if (path.empty()) return true;
        try {
            worker = std::thread([this] {
                std::unique_lock lock(workerMutex);
                while (!workerCondition.wait_for(lock, std::chrono::seconds(30), [this] { return stopWorker; })) {
                    lock.unlock();
                    try { (void)Checkpoint(); } catch (...) {}
                    lock.lock();
                }
            });
            return true;
        } catch (...) { return false; }
    }

    void StopCheckpointing() noexcept {
        {
            std::lock_guard lock(workerMutex);
            stopWorker = true;
        }
        workerCondition.notify_all();
        if (worker.joinable()) worker.join();
    }

    const std::string& Path() const { return path; }
    bool Loaded() const { return loaded; }

private:
    bool ReadSnapshot(const std::string& snapshot, std::vector<unsigned char>& data,
                      std::vector<unsigned char>& index) {
        struct stat st {};
        if (lstat(snapshot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0022) != 0) return false;
        std::vector<unsigned char> seal;
        if (!ReadFile(snapshot + "/seal", seal, 256) ||
            !ReadFile(snapshot + "/RWCacheDB.foz", data, MaxDataBytes) ||
            !ReadFile(snapshot + "/RWCacheDB_idx.foz", index, MaxIndexBytes)) return false;
        std::istringstream manifest(std::string(seal.begin(), seal.end()));
        std::string version, dataHash, indexHash, extra;
        std::uint64_t dataSize {}, indexSize {};
        if (!(manifest >> version >> dataHash >> dataSize >> indexHash >> indexSize) || manifest >> extra) return false;
        return version == "LUMEN_FEX_CACHE_V1" && Hex(dataHash, 32) && Hex(indexHash, 32) &&
               dataSize == data.size() && indexSize == index.size() && Digest(data) == dataHash &&
               Digest(index) == indexHash && ValidateDatabase(data, index);
    }

    std::string root;
    std::string path;
    bool loaded = false;
    std::size_t publishedDataSize = 0, publishedIndexSize = 0;
    std::mutex checkpointMutex;
    std::mutex workerMutex;
    std::condition_variable workerCondition;
    bool stopWorker = false;
    std::thread worker;
};

} // namespace Lumen::FexCore::TranslationCache
