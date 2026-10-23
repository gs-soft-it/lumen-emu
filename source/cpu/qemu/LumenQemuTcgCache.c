/* SPDX-License-Identifier: GPL-2.0-only */
#include "LumenQemuTcgCache.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#define CACHE_HEADER_SIZE 144
#define CACHE_ENTRY_LIMIT 512
#define CACHE_SCAN_LIMIT 4096
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int cache_directory = -1;
static int cache_quota_lock = -1;
static gint cache_write_disabled;
static uint8_t cache_build[32];
static uint64_t cache_temporary;
static LumenQemuTcgCacheStats cache_stats;
/* Only the static fields are indexed; live page bytes and permissions are
 * never retained. cache_mutex protects the snapshot and local publications. */
static GHashTable *cache_static_keys;
static bool cache_static_keys_complete;

static guint static_key_hash(gconstpointer data)
{
    const LumenQemuTcgCacheKey *key = data;
    uint64_t hash = key->pc ^ (key->cs_base * UINT64_C(0x9e3779b97f4a7c15));
    hash ^= (uint64_t)key->flags << 32 | key->cflags;
    hash ^= key->max_insns;
    return (guint)(hash ^ (hash >> 32));
}
static gboolean static_key_equal(gconstpointer first, gconstpointer second)
{
    const LumenQemuTcgCacheKey *a = first, *b = second;
    return a->pc == b->pc && a->cs_base == b->cs_base &&
        a->flags == b->flags && a->cflags == b->cflags &&
        a->max_insns == b->max_insns;
}
static void index_static_key(const LumenQemuTcgCacheKey *key)
{
    if (!cache_static_keys || g_hash_table_contains(cache_static_keys, key)) return;
    if (g_hash_table_size(cache_static_keys) >= CACHE_ENTRY_LIMIT) {
        cache_static_keys_complete = false;
        return;
    }
    LumenQemuTcgCacheKey *copy = g_new0(LumenQemuTcgCacheKey, 1);
    copy->pc = key->pc;
    copy->cs_base = key->cs_base;
    copy->flags = key->flags;
    copy->cflags = key->cflags;
    copy->max_insns = key->max_insns;
    g_hash_table_add(cache_static_keys, copy);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = value >> (i * 8);
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void hash_bytes(const uint8_t *bytes, size_t size, uint8_t result[32])
{
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(hash, bytes, size);
    gsize length = 32;
    g_checksum_get_digest(hash, result, &length);
    g_checksum_free(hash);
}
static bool private_directory(int fd)
{
    struct stat info;
    return fd >= 0 && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
        info.st_uid == geteuid() && (info.st_mode & 077) == 0;
}
static void encode_key(uint8_t header[CACHE_HEADER_SIZE], const LumenQemuTcgCacheKey *key)
{
    memset(header, 0, CACHE_HEADER_SIZE);
    memcpy(header, "LUMIR01", 8);
    memcpy(header + 8, cache_build, 32);
    put64(header + 40, key->pc);
    put64(header + 48, key->cs_base);
    put32(header + 56, key->flags);
    put32(header + 60, key->cflags);
    put32(header + 64, key->max_insns);
    memcpy(header + 72, key->page_hash, 32);
}
static void filename_for(const uint8_t *header, char filename[69])
{
    char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, header, 104);
    memcpy(filename, hash, 64);
    memcpy(filename + 64, ".lir", 5);
    g_free(hash);
}
static bool cache_filename(const char *name)
{
    if (strlen(name) != 68 || strcmp(name + 64, ".lir") != 0) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}
/* An interprocess lock covers recount and publication. A failed or truncated
 * directory scan refuses a new entry instead of guessing available quota. */
static bool cache_has_capacity(size_t *entries)
{
    *entries = 0;
    int fd = openat(cache_directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *scan = fd >= 0 ? fdopendir(fd) : NULL;
    if (!scan) {
        if (fd >= 0) close(fd);
        return false;
    }
    size_t count = 0, visited = 0;
    bool complete = false;
    while (visited++ < CACHE_SCAN_LIMIT) {
        errno = 0;
        struct dirent *entry = readdir(scan);
        if (!entry) {
            complete = errno == 0;
            break;
        }
        if (cache_filename(entry->d_name) && ++count >= CACHE_ENTRY_LIMIT) break;
    }
    closedir(scan);
    *entries = count;
    return complete && count < CACHE_ENTRY_LIMIT;
}
static bool read_all(int fd, uint8_t *bytes, size_t size)
{
    while (size) {
        ssize_t got = read(fd, bytes, size);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        bytes += got;
        size -= got;
    }
    return true;
}
static bool write_all(int fd, const uint8_t *bytes, size_t size)
{
    while (size) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written;
        size -= written;
    }
    return true;
}
/* The quota lock is held. Inspect bounded headers only; payload integrity and
 * IR validation remain on every hit. Any uncertainty disables the filter.
 * This snapshot is deliberately allowed to miss later interprocess writes:
 * a false negative falls back to JIT and cannot authorize stale execution. */
static void index_existing_keys(void)
{
    cache_static_keys = g_hash_table_new_full(static_key_hash, static_key_equal,
                                             g_free, NULL);
    int fd = openat(cache_directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *scan = fd >= 0 ? fdopendir(fd) : NULL;
    if (!scan) {
        if (fd >= 0) close(fd);
        return;
    }
    size_t visited = 0, entries = 0;
    bool complete = false, valid = true;
    while (visited++ < CACHE_SCAN_LIMIT) {
        errno = 0;
        struct dirent *entry = readdir(scan);
        if (!entry) {
            complete = errno == 0;
            break;
        }
        if (!cache_filename(entry->d_name)) continue;
        if (++entries > CACHE_ENTRY_LIMIT) { valid = false; break; }
        int record = openat(cache_directory, entry->d_name,
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat info;
        uint8_t header[CACHE_HEADER_SIZE];
        bool usable = record >= 0 && fstat(record, &info) == 0 &&
            S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            info.st_nlink == 1 && (info.st_mode & 077) == 0 &&
            info.st_size >= 104 && read_all(record, header, 104);
        if (record >= 0) close(record);
        char expected[69];
        if (usable) filename_for(header, expected);
        usable = usable && memcmp(header, "LUMIR01", 8) == 0 &&
            memcmp(header + 8, cache_build, 32) == 0 &&
            get32(header + 68) == 0 && get32(header + 64) > 0 &&
            get32(header + 64) <= 65536 && strcmp(entry->d_name, expected) == 0;
        if (!usable) { valid = false; continue; }
        LumenQemuTcgCacheKey key = {
            .pc = get64(header + 40), .cs_base = get64(header + 48),
            .flags = get32(header + 56), .cflags = get32(header + 60),
            .max_insns = get32(header + 64)
        };
        index_static_key(&key);
    }
    closedir(scan);
    cache_static_keys_complete = complete && valid;
}
/* Caller owns cache_mutex. No untrusted length drives allocation before the
 * regular-file, ownership, exact size, schema and filename checks succeed. */
static GBytes *read_entry(const char *filename, LumenQemuTcgCacheKey *key)
{
    int fd = openat(cache_directory, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) ++cache_stats.rejected;
        return NULL;
    }
    struct stat info;
    uint8_t header[CACHE_HEADER_SIZE];
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 077) == 0 &&
        info.st_size > CACHE_HEADER_SIZE &&
        info.st_size <= CACHE_HEADER_SIZE + LUMEN_QEMU_CACHE_PAYLOAD_LIMIT &&
        read_all(fd, header, sizeof(header));
    uint32_t size = valid ? get32(header + 104) : 0;
    char expected[69];
    if (valid) filename_for(header, expected);
    valid = valid && memcmp(header, "LUMIR01", 8) == 0 &&
        memcmp(header + 8, cache_build, 32) == 0 &&
        get32(header + 68) == 0 && get32(header + 108) == 0 &&
        get32(header + 64) > 0 && get32(header + 64) <= 65536 &&
        size > 0 && size <= LUMEN_QEMU_CACHE_PAYLOAD_LIMIT &&
        (uint64_t)info.st_size == CACHE_HEADER_SIZE + (uint64_t)size &&
        strcmp(filename, expected) == 0;
    uint8_t *payload = valid ? g_malloc(size) : NULL;
    uint8_t digest[32];
    if (valid) {
        valid = read_all(fd, payload, size);
        if (valid) {
            hash_bytes(payload, size, digest);
            valid = memcmp(digest, header + 112, 32) == 0;
        }
    }
    close(fd);
    if (!valid) {
        g_free(payload);
        ++cache_stats.rejected;
        return NULL;
    }
    key->pc = get64(header + 40);
    key->cs_base = get64(header + 48);
    key->flags = get32(header + 56);
    key->cflags = get32(header + 60);
    key->max_insns = get32(header + 64);
    memcpy(key->page_hash, header + 72, 32);
    return g_bytes_new_take(payload, size);
}

bool LumenQemuCacheInitialize(const char *directory, const char *build_id)
{
    if (!directory) return true;
    if (directory[0] != '/' || !build_id || strlen(build_id) != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        int a = g_ascii_xdigit_value(build_id[i * 2]);
        int b = g_ascii_xdigit_value(build_id[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        cache_build[i] = (a << 4) | b;
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return false;
    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (!private_directory(parent)) {
        if (parent >= 0) close(parent);
        return false;
    }
    if (mkdirat(parent, build_id, 0700) != 0 && errno != EEXIST) {
        close(parent);
        return false;
    }
    int fd = openat(parent, build_id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(parent);
    if (!private_directory(fd)) {
        if (fd >= 0) close(fd);
        return false;
    }
    int quota = openat(fd, ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat lock_info;
    if (quota < 0 || fstat(quota, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != geteuid() || lock_info.st_nlink != 1 || (lock_info.st_mode & 077)) {
        if (quota >= 0) close(quota);
        close(fd);
        return false;
    }
    cache_directory = fd;
    cache_quota_lock = quota;
    g_atomic_int_set(&cache_write_disabled, 0);
    if (flock(cache_quota_lock, LOCK_EX | LOCK_NB) == 0) {
        index_existing_keys();
        size_t entries;
        cache_has_capacity(&entries);
        if (entries >= CACHE_ENTRY_LIMIT) g_atomic_int_set(&cache_write_disabled, 1);
        flock(cache_quota_lock, LOCK_UN);
    }
    return true;
}
bool LumenQemuCacheEnabled(void) { return cache_directory >= 0; }
bool LumenQemuCacheMayStore(void)
{
    return LumenQemuCacheEnabled() && !g_atomic_int_get(&cache_write_disabled);
}
bool LumenQemuCacheMayLoad(const LumenQemuTcgCacheKey *key)
{
    if (!LumenQemuCacheEnabled()) return false;
    if (!g_atomic_int_get(&cache_write_disabled)) return true;
    pthread_mutex_lock(&cache_mutex);
    bool possible = !cache_static_keys_complete ||
        g_hash_table_contains(cache_static_keys, key);
    if (!possible) ++cache_stats.misses;
    pthread_mutex_unlock(&cache_mutex);
    return possible;
}
GBytes *LumenQemuCacheLoad(const LumenQemuTcgCacheKey *key)
{
    uint8_t header[CACHE_HEADER_SIZE];
    char filename[69];
    encode_key(header, key);
    filename_for(header, filename);
    pthread_mutex_lock(&cache_mutex);
    LumenQemuTcgCacheKey found;
    GBytes *bytes = read_entry(filename, &found);
    if (!bytes) ++cache_stats.misses;
    pthread_mutex_unlock(&cache_mutex);
    return bytes;
}
void LumenQemuCacheStore(const LumenQemuTcgCacheKey *key, const uint8_t *ir, size_t size)
{
    if (!LumenQemuCacheMayStore() || !size || size > LUMEN_QEMU_CACHE_PAYLOAD_LIMIT) return;
    uint8_t header[CACHE_HEADER_SIZE];
    char filename[69], temporary[96];
    encode_key(header, key);
    filename_for(header, filename);
    put32(header + 104, size);
    hash_bytes(ir, size, header + 112);
    pthread_mutex_lock(&cache_mutex);
    if (g_atomic_int_get(&cache_write_disabled)) {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    if (flock(cache_quota_lock, LOCK_EX | LOCK_NB) != 0) {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    struct stat info;
    bool exists = fstatat(cache_directory, filename, &info, AT_SYMLINK_NOFOLLOW) == 0;
    size_t entries = 0;
    if (!exists && !cache_has_capacity(&entries)) {
        if (entries >= CACHE_ENTRY_LIMIT) g_atomic_int_set(&cache_write_disabled, 1);
        flock(cache_quota_lock, LOCK_UN);
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    snprintf(temporary, sizeof(temporary), ".write-%ld-%llu", (long)getpid(),
             (unsigned long long)++cache_temporary);
    int fd = openat(cache_directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool stored = fd >= 0 && write_all(fd, header, sizeof(header)) && write_all(fd, ir, size);
    if (fd >= 0 && close(fd) != 0) stored = false;
    if (stored) stored = renameat(cache_directory, temporary, cache_directory, filename) == 0;
    if (!stored) unlinkat(cache_directory, temporary, 0);
    else {
        ++cache_stats.stores;
        index_static_key(key);
        if (!exists && entries + 1 >= CACHE_ENTRY_LIMIT)
            g_atomic_int_set(&cache_write_disabled, 1);
    }
    flock(cache_quota_lock, LOCK_UN);
    pthread_mutex_unlock(&cache_mutex);
}
void LumenQemuCacheHit(void)
{
    pthread_mutex_lock(&cache_mutex);
    ++cache_stats.hits;
    pthread_mutex_unlock(&cache_mutex);
}
void LumenQemuCacheReject(void)
{
    pthread_mutex_lock(&cache_mutex);
    ++cache_stats.rejected;
    ++cache_stats.misses;
    pthread_mutex_unlock(&cache_mutex);
}
void LumenQemuCacheStats(LumenQemuTcgCacheStats *stats)
{
    pthread_mutex_lock(&cache_mutex);
    stats->hits = cache_stats.hits;
    stats->misses = cache_stats.misses;
    stats->stores = cache_stats.stores;
    stats->rejected = cache_stats.rejected;
    stats->pretranslated = 0; /* Reserved status: no ahead path in this baseline. */
    pthread_mutex_unlock(&cache_mutex);
}
