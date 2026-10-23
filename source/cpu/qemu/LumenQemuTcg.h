#ifndef LUMEN_QEMU_TCG_H
#define LUMEN_QEMU_TCG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LumenQemuTcgSession LumenQemuTcgSession;

enum LumenQemuTcgResult {
    LUMEN_QEMU_TCG_READY = 0,
    LUMEN_QEMU_TCG_HOST_CALL = 1,
    LUMEN_QEMU_TCG_RETURNED = 2,
    LUMEN_QEMU_TCG_INTERRUPTED = 3,
    LUMEN_QEMU_TCG_SYSCALL = 4,
    LUMEN_QEMU_TCG_GUEST_FAULT = 5,
    LUMEN_QEMU_TCG_INVALID_ARGUMENT = 6,
    LUMEN_QEMU_TCG_CAPACITY = 7,
    LUMEN_QEMU_TCG_INTERNAL_ERROR = 8
};

enum LumenQemuTcgMemoryPermission {
    LUMEN_QEMU_TCG_READ = 1,
    LUMEN_QEMU_TCG_WRITE = 2,
    LUMEN_QEMU_TCG_EXECUTE = 4
};

typedef struct LumenQemuTcgGuestState {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t rip;
    /* Architectural encoding order: RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
       R8 through R15. This is the same encoding order used by FEX. */
    uint64_t gprs[16];
    uint64_t rflags;
    uint64_t fs_base;
    uint64_t gs_base;
    uint8_t ymm[16][32];
    uint32_t mxcsr;
    uint16_t x87_control;
    uint16_t reserved;
    uint64_t fault_address;
    uint64_t boundary_target;
    uint32_t fault_exception;
    uint32_t fault_access;
} LumenQemuTcgGuestState;

typedef struct LumenQemuTcgMemoryRegion {
    uint64_t guest_base;
    uint64_t size;
    void *host_base;
    uint32_t permissions;
    uint32_t reserved;
} LumenQemuTcgMemoryRegion;

enum LumenQemuTcgMemoryProof {
    /* Optional bit in MemoryRegion.reserved. The memory owner proves that
     * every byte of this entire returned range has no CPU, GPU or native
     * writable physical alias while the caller holds its ordinary VM lease.
     * Structural/rights changes and executable writes still require the
     * mutation barrier and Invalidate before guest execution resumes.
     * A read-only virtual mapping alone never establishes this proof. */
    LUMEN_QEMU_TCG_IMMUTABLE_CODE = 1
};

/* Queries borrow existing Lumen memory. They must return before the core
   raises an exception; no HLE work is performed from these callbacks.
   The owner must keep mapped bytes and the queried permissions stable until
   all sessions reach a boundary and LumenQemuTcgInvalidate completes. */
typedef int (*LumenQemuTcgMemoryQuery)(void *opaque, uint64_t address,
                                     uint64_t size, uint32_t access,
                                     LumenQemuTcgMemoryRegion *region);
typedef int (*LumenQemuTcgHostTargetQuery)(void *opaque, uint64_t pc);

enum LumenQemuTcgRunFlag {
    /* The callback and everything it invokes must preserve this thread's
       JIT write/execute protection, including on every return path. */
    LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT = 1u
};

typedef struct LumenQemuTcgRunConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t return_target;
    LumenQemuTcgMemoryQuery memory_query;
    LumenQemuTcgHostTargetQuery host_target_query;
    void *opaque;
    /* ABI v2. ABI v1 remains accepted with exactly the prefix ending before
       flags; its callbacks make no protection-preservation promise. */
    uint32_t flags;
    uint32_t reserved;
} LumenQemuTcgRunConfig;

/* Initialize exactly the selected provider before starting guest threads. */
/* Process-latched CHAINING=0|1 and PARALLEL=0|1 use the environment
   variables LUMEN_QEMU_TCG_CHAINING and LUMEN_QEMU_TCG_PARALLEL. Both default
   to 1; explicit 0 selects the previous serial/unchained path. Malformed values deny
   initialization. Chaining retains HLE/return/quantum checks in every TB. */
/* LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK=0|1 is process-latched, default 0.
   Explicit 1 is supported only on macOS and selects CLOCK_MONOTONIC_RAW for
   the host Run quantum deadline/checks. It includes system sleep and excludes
   frequency/time adjustments. Guest TSC and diagnostic clocks are unchanged;
   every TB still checks the quantum. Unsupported/malformed choices deny init. */
/* LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING=0|1 is process-latched, default 0.
   Explicit 1 merges consecutive identical, no-data TCG topology flush jobs
   only while they remain at the same CPU work queue's tail. Intervening and
   dequeued work, every exit request, full TLB invalidation and RCU fences are
   preserved. Malformed choices deny initialization. */
uint32_t LumenQemuTcgInitialize(uint32_t max_host_threads);
const char *LumenQemuTcgReceipt(void);
uint64_t LumenQemuTcgTscFrequency(void);
uint64_t LumenQemuTcgReadTsc(void);

/* Process-latched persistent scalar IR defaults ON. IR_CACHE=0|1 is the
 * LUMEN_QEMU_TCG_IR_CACHE rollback selector. An absolute, private
 * LUMEN_QEMU_TCG_IR_CACHE_DIR overrides the host cache directory. Desktop
 * uses its user cache location; Android must supply the app cache directory
 * before initialization. There is no ahead-of-execution path. Legacy
 * PRETRANSLATE=0 is accepted; other values deny initialization. Stored IR is
 * rebuilt by the current native backend during execution, never native code
 * loaded from disk. The reserved pretranslated status counter remains zero. */
typedef struct LumenQemuTcgCacheStats {
    uint32_t struct_size, abi_version;
    uint64_t hits, misses, stores, rejected, pretranslated;
} LumenQemuTcgCacheStats;
uint32_t LumenQemuTcgReadCacheStats(LumenQemuTcgCacheStats *stats);

/* Open/Run/Close stay on the same native host thread. Nested sessions own
   separate CPU state. HLE dispatch happens after Run returns HOST_CALL.
   Run may return INTERRUPTED at a scheduling quantum without an explicit
   interrupt. Resume using the returned state after the owner's VM safepoint.
   First Open, Run and Invalidate share FIFO admission. The process-latched
   LUMEN_QEMU_TCG_PARALLEL=1 default admits consecutive Runs concurrently;
   Open, Invalidate and verification operations remain exclusive. Explicit 0
   preserves serial admission. */
uint32_t LumenQemuTcgOpen(const LumenQemuTcgRunConfig *config,
                         LumenQemuTcgSession **session);
uint32_t LumenQemuTcgRun(LumenQemuTcgSession *session,
                        LumenQemuTcgGuestState *state);
void LumenQemuTcgClose(LumenQemuTcgSession *session);

/* Interrupt may be called from another thread. Invalidate requires the
   owner's VM mutation barrier: no session executing or starting execution. */
void LumenQemuTcgInterrupt(LumenQemuTcgSession *session);
/* DEFER_RECLAIM=0|1 is process-latched, default 0. Full TB/TLB invalidation,
 * topology removal and queued CPU work always complete before return. In
 * mode 1, only proven canonical preallocated RAM metadata with no removal
 * callbacks may be reclaimed asynchronously; borrowed bytes can immediately
 * be protected, unmapped or reused after return. Sixteen retirement batches,
 * 512 RAM views, or 256 MiB of RAM-offset metadata extent trigger a two-stage
 * RCU fence; oversized single batches are synchronous. Failed proofs preserve
 * synchronous reclamation. This never relaxes VM/physical-alias admission. */
uint32_t LumenQemuTcgInvalidate(uint64_t address, uint64_t size);

/* Publish a proven additive VM mapping under the same mutation barrier.
   The caller must establish that no existing backing or rights change,
   including through physical aliases outside this range. If the complete
   range misses every admitted RAM view, no live TB/TLB can reference it;
   preserve those translations. Any intersection uses global invalidation.
   This operation retains exclusive FIFO core admission even on a miss. */
uint32_t LumenQemuTcgInvalidateAdditive(uint64_t address, uint64_t size);

#ifdef __cplusplus
}
#endif
#endif
