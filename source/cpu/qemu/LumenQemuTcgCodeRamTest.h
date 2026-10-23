/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUMEN_QEMU_TCG_CODE_RAM_TEST_H
#define LUMEN_QEMU_TCG_CODE_RAM_TEST_H

/* Native fixture seam, present only in the separately built verification
 * library. Production libraries contain neither these exports nor counters. */
#include "LumenQemuTcg.h"

typedef struct LumenQemuTcgCodeRamStats {
    uint64_t fast;
    uint64_t fallback;
    uint64_t reference;
    uint64_t padded;
    uint64_t original_divergence;
    uint64_t mismatch;
} LumenQemuTcgCodeRamStats;

typedef struct LumenQemuTcgCodeRamProbe {
    uint64_t ram_offset;
    uint64_t canonical_offset;
    uint64_t original_offset;
    uint64_t block_length;
    uint64_t view_length;
    uint32_t owner_resolved;
    uint32_t victim_present;
    uint32_t warm_owner_resolved;
    uint32_t warm_rejected;
} LumenQemuTcgCodeRamProbe;

typedef struct LumenQemuTcgReclaimStats {
    uint64_t deferred_batches;
    uint64_t fence_pairs;
    uint64_t guard_fallbacks;
    uint64_t pending_maps;
    uint64_t pending_bytes;
    uint64_t notifier_removals;
    uint64_t notifier_failures;
    uint32_t pending_batches;
    uint32_t max_pending_batches;
    uint32_t batch_limit;
    uint32_t reserved;
} LumenQemuTcgReclaimStats;

/* A dedicated non-CPU fixture thread can retain an RCU generation without
 * borrowing guest bytes, forcing old metadata to survive immediate VA reuse. */
uint32_t LumenQemuTcgReclaimTestRcuHold(uint32_t begin);
void LumenQemuTcgReclaimTestDrain(void);
void LumenQemuTcgReclaimTestRead(LumenQemuTcgReclaimStats *stats);
/* Removal callback reads a sentinel byte, so its backing must remain RW
 * until the synchronous fallback has completed and the seam is disabled. */
uint32_t LumenQemuTcgReclaimTestNotifier(uint32_t enable, void *host, uint64_t size);

/* Force the independent full-TLB canonical-offset reference calculation;
 * original_offset remains diagnostic and may differ under host padding. */
void LumenQemuTcgCodeRamTestReset(uint32_t force_reference);
void LumenQemuTcgCodeRamTestRead(LumenQemuTcgCodeRamStats *stats);
uint32_t LumenQemuTcgCodeRamTestProbe(LumenQemuTcgSession *session,
    uint64_t address, const LumenQemuTcgMemoryRegion *add_after_warm,
    LumenQemuTcgCodeRamProbe *result);
uint32_t LumenQemuTcgCodeRamTestWritableVictim(LumenQemuTcgSession *session,
                                             uint64_t address);
/* Real CPU queue effects verify A/A, intervening A/W/A, a successor queued
 * from its running predecessor, and two RAM replacements before one drain.
 * Always tests a private copied CPU-ops opt-in, regardless of process policy. */
uint32_t LumenQemuTcgCodeRamTestQueueCoalescing(void);

#endif
