/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUMEN_QEMU_TCG_CACHE_H
#define LUMEN_QEMU_TCG_CACHE_H

#include "LumenQemuTcg.h"
#include <glib.h>
#include <stdbool.h>

#define LUMEN_QEMU_CACHE_PAYLOAD_LIMIT (64 * 1024)

typedef struct LumenQemuTcgCacheKey {
    uint64_t pc, cs_base;
    uint32_t flags, cflags, max_insns;
    uint8_t page_hash[32];
} LumenQemuTcgCacheKey;

/* Explicit directory configuration is private, process-latched and bounded.
 * The stored product is canonical TCG IR; native code is emitted anew by the
 * running QEMU backend. Missing/stale/corrupt entries are ordinary JIT misses. */
bool LumenQemuCacheInitialize(const char *directory, const char *build_id);
bool LumenQemuCacheEnabled(void);
/* Once the interprocess quota is observed full, this process keeps loading
 * records but skips all persistent writes and their IR preparation. */
bool LumenQemuCacheMayStore(void);
/* A complete process-local snapshot can rule out the static part of a key
 * once the writer quota is full. A positive result grants no permission to
 * import: the caller still proves and hashes live code, then validates IR.
 * Concurrent publications may be conservatively missed until next process. */
bool LumenQemuCacheMayLoad(const LumenQemuTcgCacheKey *key);
GBytes *LumenQemuCacheLoad(const LumenQemuTcgCacheKey *key);
void LumenQemuCacheStore(const LumenQemuTcgCacheKey *key, const uint8_t *ir, size_t size);
void LumenQemuCacheHit(void);
void LumenQemuCacheReject(void);
void LumenQemuCacheStats(LumenQemuTcgCacheStats *stats);

#endif
