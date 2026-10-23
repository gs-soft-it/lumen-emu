/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "LumenQemuTcg.h"
#include "LumenQemuTcgCache.h"
#include "qemu-main.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "system/system.h"
#include "system/cpus.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/ramblock.h"
#include "system/ramlist.h"
#include "system/replay.h"
#include "accel/accel-cpu-ops.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/tcg-accel-ops.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "target/i386/cpu.h"
#include "target/i386/tcg/helper-tcg.h"
#include "tcg/startup.h"
#include "tcg/tcg.h"
#include "tcg/lumen-ir-cache.h"

#ifndef LUMEN_QEMU_IR_CACHE_BUILD_ID
#error "The persistent IR cache requires a checked build identity"
#endif

#define LUMEN_TCG_MAX_HOST_THREADS 256
#define LUMEN_TCG_MAX_NESTING 32
#define LUMEN_TCG_SYSCALL_PC UINT64_C(0xfffffffffffff000)
#define LUMEN_TCG_PAGE_SIZE UINT64_C(4096)
#define LUMEN_TCG_QUANTUM_NS INT64_C(2000000)
#define LUMEN_TCG_MIN_QUANTUM_NS INT64_C(250000)
#define LUMEN_TCG_RECLAIM_BATCH_LIMIT 16
#define LUMEN_TCG_RECLAIM_MAP_LIMIT 512
#define LUMEN_TCG_RECLAIM_BYTE_LIMIT (UINT64_C(256) * 1024 * 1024)

int (*qemu_main)(void);

struct LumenQemuTcgSession {
    CPUState *cpu;
    CPUClass cpu_class;
    TCGCPUOps tcg_ops;
    LumenQemuTcgRunConfig config;
    QemuThread owner;
    uint32_t boundary;
    uint32_t interrupt_requested;
    uint64_t fault_address;
    uint32_t fault_exception;
    uint32_t fault_access;
    int64_t deadline;
    bool open;
    bool executing;
};

typedef struct LumenQemuTcgMap {
    Object parent_obj;
    MemoryRegion memory;
    LumenQemuTcgMemoryRegion view;
    struct LumenQemuTcgMap *next;
} LumenQemuTcgMap;

static const TypeInfo lumen_map_type = {
    .name = "lumen-tcg-owned-memory",
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(LumenQemuTcgMap),
};
static void lumen_register_map_type(void) { type_register_static(&lumen_map_type); }
type_init(lumen_register_map_type)

static uint32_t initialized;
static QemuMutex execution_mutex;
static uint64_t execution_next_ticket;
static uint64_t execution_serving_ticket;
static bool parallel_execution;
static bool block_chaining;
static uint32_t execution_readers;
static bool execution_writer;
static bool fair_budget_scheduler;
static bool scheduler_trace;
static bool raw_quantum_clock;
static bool tlb_commit_coalescing;
static bool jit_exec_cache;
static bool defer_reclaim;
static bool ir_cache_trace;
static uint64_t cache_key_queries, cache_key_unmapped, cache_key_writable;
static uint64_t cache_key_unproven, cache_export_unsupported;
/* Exclusive FIFO admission protects this conservative count. Completed
 * asynchronous batches may still count until the next synchronous fence. */
static uint32_t pending_reclaim_batches;
static uint64_t pending_reclaim_maps;
static uint64_t pending_reclaim_bytes;
typedef struct LumenExecutionWaiter {
    QemuCond changed;
    uint64_t ticket;
    struct LumenExecutionWaiter *next;
} LumenExecutionWaiter;
static LumenExecutionWaiter *execution_waiters;
static LumenExecutionWaiter *execution_waiters_tail;
enum LumenExecutionOperation { LUMEN_OP_RUN, LUMEN_OP_OPEN, LUMEN_OP_INVALIDATE, LUMEN_OP_VERIFY };
typedef struct LumenExecutionTrace {
    uint64_t admitted[4], execution_ns[4];
    uint64_t blocked, wait_calls, signals, completed, max_backlog;
    uint64_t wait_ns, max_wait_ns, max_execution_ns;
    QemuThreadJitStats jit;
} LumenExecutionTrace;
static LumenExecutionTrace execution_trace;
static __thread enum LumenExecutionOperation execution_operation;
static __thread int64_t execution_started;
static int64_t execution_last_trace;
static AccelOpsClass accel_ops;
static uint32_t max_threads;
static uint32_t registered_threads;
static uint32_t next_apic_id;
static bool bootstrap_realizing;
static LumenQemuTcgMap *maps;
static ObjectClass *map_class;
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
#include "LumenQemuTcgCodeRamTest.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/probe.h"
#include "qemu/memalign.h"
static LumenQemuTcgCodeRamStats code_ram_stats;
static bool code_ram_force_reference;
static LumenQemuTcgReclaimStats reclaim_stats;
static __thread bool reclaim_test_read_locked;
static RAMBlockNotifier reclaim_test_notifier;
static void *reclaim_test_host;
static uint64_t reclaim_test_size;
static bool reclaim_test_notifier_registered;
#endif
static TCGTBCPUState (*original_get_tb_state)(CPUState *cpu);
static __thread bool thread_registered;
static __thread bool thread_has_rcu;
static __thread bool thread_owns_rcu;
static __thread LumenQemuTcgSession *thread_sessions[LUMEN_TCG_MAX_NESTING];
static __thread uint32_t thread_depth;
static __thread LumenQemuTcgSession *active_session;

/* The Apple nanosecond API uses the same CLOCK_MONOTONIC domain as QEMU's
 * get_clock. Preserve QEMU's runtime fallback if that clock was unavailable.
 * The ordinary quantum and admission diagnostics use this helper; guest TSC
 * stays get_clock even when a Run explicitly selects the raw clock below. */
static inline int64_t lumen_quantum_clock(void)
{
#ifdef __APPLE__
    if (use_rt_clock) {
        return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
    }
#endif
    return get_clock();
}

/* Only a Run's deadline and its TB checks use this opt-in clock. Darwin RAW
 * advances during system sleep, but does not follow frequency/time adjustments.
 * Admission diagnostics and the guest TSC retain the existing clock domain. */
static inline int64_t lumen_run_clock(void)
{
#ifdef __APPLE__
    if (raw_quantum_clock) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        if (!now || now > INT64_MAX - LUMEN_TCG_QUANTUM_NS) {
            fprintf(stderr, "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK_V1 clock=failed execution=denied\n");
            abort();
        }
        return (int64_t)now;
    }
#endif
    return lumen_quantum_clock();
}

static void lumen_jit_scope_begin(void)
{
    qemu_thread_jit_scope_begin(jit_exec_cache, scheduler_trace);
}

static void lumen_jit_scope_end(void)
{
    /* JIT scopes are per native thread. Merge their completed contribution
     * under the admission mutex before leave can publish a trace snapshot. */
    if (scheduler_trace) {
        QemuThreadJitStats stats;
        qemu_thread_jit_scope_read(&stats);
        qemu_mutex_lock(&execution_mutex);
        execution_trace.jit.execute_calls += stats.execute_calls;
        execution_trace.jit.execute_skipped += stats.execute_skipped;
        execution_trace.jit.write_calls += stats.write_calls;
        execution_trace.jit.callback_restore_write += stats.callback_restore_write;
        execution_trace.jit.callback_restore_execute += stats.callback_restore_execute;
        execution_trace.jit.callback_restore_unknown += stats.callback_restore_unknown;
        qemu_mutex_unlock(&execution_mutex);
    }
    qemu_thread_jit_scope_end();
}

static void lumen_execution_signal_locked(void)
{
    if (execution_waiters) {
        assert(execution_waiters->ticket == execution_serving_ticket);
        qemu_cond_signal(&execution_waiters->changed);
        if (scheduler_trace) ++execution_trace.signals;
    }
}

static bool lumen_execution_blocked(uint64_t ticket,
                                    enum LumenExecutionOperation operation)
{
    if (ticket != execution_serving_ticket) return true;
    if (!parallel_execution) return false;
    return execution_writer ||
        (operation != LUMEN_OP_RUN && execution_readers != 0);
}

/* Preserve FIFO admission, including first Open and invalidation. Parallel
 * Run readers can overlap until a queued exclusive operation reaches the
 * head. That writer closes admission and drains existing readers before
 * realization, proof fixtures or invalidation can touch other CPU contexts. */
static uint64_t lumen_execution_enter(enum LumenExecutionOperation operation)
{
    const int64_t queued_at = scheduler_trace ? lumen_quantum_clock() : 0;
    qemu_mutex_lock(&execution_mutex);
    const uint64_t ticket = execution_next_ticket++;
    const bool blocked = lumen_execution_blocked(ticket, operation);
    LumenExecutionWaiter waiter = {.ticket = ticket};
    if (blocked) {
        /* A waiter owns its condition until it removes itself while holding
         * the mutex. Only the head is signalled; spurious wakes still check
         * the authoritative ticket. No pointer survives this enter call. */
        qemu_cond_init(&waiter.changed);
        if (execution_waiters_tail) execution_waiters_tail->next = &waiter;
        else execution_waiters = &waiter;
        execution_waiters_tail = &waiter;
        if (scheduler_trace) ++execution_trace.blocked;
        while (lumen_execution_blocked(ticket, operation)) {
            if (scheduler_trace) ++execution_trace.wait_calls;
            qemu_cond_wait(&waiter.changed, &execution_mutex);
        }
        assert(execution_waiters == &waiter);
        execution_waiters = waiter.next;
        if (!execution_waiters) execution_waiters_tail = NULL;
    }
    /* Includes this operation and the contenders already queued at admission.
     * Open/Invalidate remain in the same FIFO; they conservatively shorten a
     * Run's budget too. This is an instantaneous queue snapshot, not a count
     * of all runnable guest threads or a bound on subsequent admission time. */
    const uint64_t backlog = execution_next_ticket - execution_serving_ticket +
        (parallel_execution ? execution_readers : 0);
    execution_operation = operation;
    if (scheduler_trace) {
        const int64_t admitted_at = lumen_quantum_clock();
        const uint64_t wait = MAX(INT64_C(0), admitted_at - queued_at);
        ++execution_trace.admitted[operation];
        execution_trace.wait_ns += wait;
        execution_trace.max_wait_ns = MAX(execution_trace.max_wait_ns, wait);
        execution_trace.max_backlog = MAX(execution_trace.max_backlog, backlog);
        execution_started = admitted_at;
    }
    if (parallel_execution) {
        ++execution_serving_ticket;
        if (operation == LUMEN_OP_RUN) {
            ++execution_readers;
            /* Admit the next reader without waiting for this Run to finish.
             * An exclusive head still waits for every active reader. */
            lumen_execution_signal_locked();
        } else {
            execution_writer = true;
        }
    }
    qemu_mutex_unlock(&execution_mutex);
    if (blocked) qemu_cond_destroy(&waiter.changed);
    return backlog;
}

static int64_t lumen_run_quantum(uint64_t backlog)
{
    if (!fair_budget_scheduler) {
        return LUMEN_TCG_QUANTUM_NS;
    }
    /* A queued operation includes itself, so backlog cannot be zero. Saturate
     * the divisor at eight: more contenders keep the 250 us floor. Neither
     * Open/Invalidate count nor host scheduling supplies a 2 ms round bound. */
    const int64_t divisor = (int64_t)MIN(MAX(backlog, UINT64_C(1)), UINT64_C(8));
    return MAX(LUMEN_TCG_MIN_QUANTUM_NS, LUMEN_TCG_QUANTUM_NS / divisor);
}

static void lumen_execution_leave(void)
{
    LumenExecutionTrace snapshot;
    bool emit = false;
    qemu_mutex_lock(&execution_mutex);
    if (scheduler_trace) {
        const int64_t now = lumen_quantum_clock();
        const uint64_t elapsed = MAX(INT64_C(0), now - execution_started);
        execution_trace.execution_ns[execution_operation] += elapsed;
        execution_trace.max_execution_ns = MAX(execution_trace.max_execution_ns, elapsed);
        emit = (++execution_trace.completed % 256) == 0 &&
            (!execution_last_trace || now - execution_last_trace >= INT64_C(1000000000));
        if (emit) execution_last_trace = now;
    }
    if (parallel_execution) {
        if (execution_operation == LUMEN_OP_RUN) {
            assert(execution_readers != 0 && !execution_writer);
            --execution_readers;
            if (!execution_readers) lumen_execution_signal_locked();
        } else {
            assert(execution_writer && execution_readers == 0);
            execution_writer = false;
            lumen_execution_signal_locked();
        }
    } else {
        ++execution_serving_ticket;
        lumen_execution_signal_locked();
    }
    if (emit) snapshot = execution_trace;
    qemu_mutex_unlock(&execution_mutex);
    if (emit) {
        fprintf(stderr, "LUMEN_QEMU_TCG_SCHED_TRACE_V1 wake=targeted completed=%" PRIu64
            " run=%" PRIu64 " open=%" PRIu64 " invalidate=%" PRIu64 " verify=%" PRIu64
            " blocked=%" PRIu64 " wait_calls=%" PRIu64 " signals=%" PRIu64
            " max_backlog=%" PRIu64 " wait_ns=%" PRIu64 " max_wait_ns=%" PRIu64
            " run_ns=%" PRIu64 " open_ns=%" PRIu64 " invalidate_ns=%" PRIu64
            " verify_ns=%" PRIu64 " max_execution_ns=%" PRIu64
            " jit_execute=%" PRIu64 " jit_skip=%" PRIu64 " jit_write=%" PRIu64
            " callback_write=%" PRIu64 " callback_execute=%" PRIu64
            " callback_unknown=%" PRIu64 " diagnostic=1\n",
            snapshot.completed, snapshot.admitted[LUMEN_OP_RUN], snapshot.admitted[LUMEN_OP_OPEN],
            snapshot.admitted[LUMEN_OP_INVALIDATE], snapshot.admitted[LUMEN_OP_VERIFY],
            snapshot.blocked, snapshot.wait_calls, snapshot.signals, snapshot.max_backlog,
            snapshot.wait_ns, snapshot.max_wait_ns, snapshot.execution_ns[LUMEN_OP_RUN],
            snapshot.execution_ns[LUMEN_OP_OPEN], snapshot.execution_ns[LUMEN_OP_INVALIDATE],
            snapshot.execution_ns[LUMEN_OP_VERIFY], snapshot.max_execution_ns,
            snapshot.jit.execute_calls, snapshot.jit.execute_skipped, snapshot.jit.write_calls,
            snapshot.jit.callback_restore_write, snapshot.jit.callback_restore_execute,
            snapshot.jit.callback_restore_unknown);
    }
}

static void lumen_machine_init(MachineState *machine) { }

static void lumen_machine_class_init(MachineClass *machine)
{
    machine->desc = "Lumen shared x86-64 CPU translation environment";
    machine->init = lumen_machine_init;
    machine->max_cpus = LUMEN_TCG_MAX_HOST_THREADS;
    machine->default_ram_size = 0;
    machine->no_serial = 1;
    machine->no_parallel = 1;
    machine->no_floppy = 1;
    machine->no_cdrom = 1;
}
DEFINE_MACHINE("lumen-tcg", lumen_machine_class_init)

uint64_t LumenQemuTcgTscFrequency(void) { return UINT64_C(1000000000); }
uint64_t LumenQemuTcgReadTsc(void) { return (uint64_t)get_clock(); }
static int64_t lumen_elapsed_ticks(void) { return get_clock(); }

static void lumen_create_vcpu(CPUState *cpu)
{
    tcg_cpu_init_cflags(cpu, true);
    tcg_cflags_set(cpu, cpu->tcg_cflags | (block_chaining ?
        CF_TB_EXEC_BOUNDARY : CF_NO_GOTO_TB | CF_NO_GOTO_PTR));
    if (!bootstrap_realizing && !thread_registered) {
        tcg_register_thread();
        thread_registered = true;
        ++registered_threads;
    }
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);
}

static void lumen_kick_vcpu(CPUState *cpu)
{
    qatomic_store_release(&cpu->exit_request, true);
    qatomic_set(&cpu->neg.icount_decr.u16.high, -1);
}

static G_NORETURN void lumen_exit(CPUState *cpu, uint32_t boundary)
{
    active_session->boundary = boundary;
    cpu->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cpu);
}

static uint32_t lumen_boundary(CPUState *cpu, uint64_t pc)
{
    LumenQemuTcgSession *session = active_session;
    if (pc == session->config.return_target) {
        return LUMEN_QEMU_TCG_RETURNED;
    }
    if (pc == LUMEN_TCG_SYSCALL_PC) {
        return LUMEN_QEMU_TCG_SYSCALL;
    }
    if (session->config.host_target_query) {
        const bool preserves = session->config.flags & LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT;
        const unsigned int jit_mode = block_chaining && !preserves ?
            qemu_thread_jit_callback_begin() : QEMU_THREAD_JIT_UNKNOWN;
        if (!block_chaining && !preserves) qemu_thread_jit_invalidate();
        const int target = session->config.host_target_query(session->config.opaque, pc);
        if (!preserves) {
            if (block_chaining) qemu_thread_jit_callback_end(jit_mode);
            /* A linked TB or lookup helper can return directly into MAP_JIT
             * code even when transition caching is disabled. A legacy
             * callback may have switched this native thread to WRITE. */
            if (block_chaining && jit_mode != QEMU_THREAD_JIT_EXECUTE)
                qemu_thread_jit_execute();
            else qemu_thread_jit_invalidate();
        }
        if (target) return LUMEN_QEMU_TCG_HOST_CALL;
    }
    if (lumen_run_clock() >= session->deadline) {
        return LUMEN_QEMU_TCG_INTERRUPTED;
    }
    return LUMEN_QEMU_TCG_READY;
}

static void lumen_tb_exec_boundary(CPUState *cpu, const TranslationBlock *tb)
{
    /* The entry helper materializes TCG globals, including lazy CC inputs.
     * Direct chaining may leave non-PCREL EIP at the preceding block. Only
     * synchronize PC on an exit; no guest instruction has begun, so an
     * instruction-return-address unwind is not applicable at this point. */
    const uint64_t pc = (tb_cflags(tb) & CF_PCREL) ? cpu_env(cpu)->eip : tb->pc;
    const uint32_t boundary = lumen_boundary(cpu, pc);
    if (boundary != LUMEN_QEMU_TCG_READY) {
        cpu->cc->tcg_ops->synchronize_from_tb(cpu, tb);
        lumen_exit(cpu, boundary);
    }
}

static TCGTBCPUState lumen_get_tb_state(CPUState *cpu)
{
    /* Uncached native HLE and return sentinels must be intercepted before
     * fetching guest instructions. Warm linked targets use the entry hook. */
    const uint32_t boundary = lumen_boundary(cpu, cpu_env(cpu)->eip);
    if (boundary != LUMEN_QEMU_TCG_READY) lumen_exit(cpu, boundary);
    return original_get_tb_state(cpu);
}

static void lumen_interrupt(CPUState *cpu)
{
    LumenQemuTcgSession *session = active_session;
    session->fault_exception = cpu->exception_index;
    if (cpu->exception_index == EXCP0E_PAGE) {
        session->fault_address = cpu_env(cpu)->cr[2];
    }
    lumen_exit(cpu, LUMEN_QEMU_TCG_GUEST_FAULT);
}

static bool lumen_region_valid(const LumenQemuTcgMemoryRegion *region,
                               uint64_t page, uint32_t access)
{
    const uint64_t mask = LUMEN_TCG_PAGE_SIZE - 1;
    if (!region->host_base || !region->size ||
        ((region->guest_base | region->size | (uintptr_t)region->host_base) & mask) ||
        region->guest_base > UINT64_MAX - region->size ||
        (uintptr_t)region->host_base > UINTPTR_MAX - region->size ||
        page < region->guest_base || page - region->guest_base >= region->size ||
        region->size - (page - region->guest_base) < LUMEN_TCG_PAGE_SIZE ||
        (region->permissions & access) != access || (region->permissions & ~7u)) {
        return false;
    }
    return true;
}

static bool lumen_ir_key(CPUState *cpu, const TranslationBlock *tb, uint64_t pc,
                         int max_insns, LumenQemuTcgCacheKey *key)
{
    LumenQemuTcgSession *session = active_session;
    if (!LumenQemuCacheEnabled() || !session || session->cpu != cpu ||
        max_insns <= 0 || max_insns > TCG_MAX_INSNS ||
        pc == session->config.return_target || pc == LUMEN_TCG_SYSCALL_PC) return false;
    if (ir_cache_trace) qatomic_inc(&cache_key_queries);
    const uint64_t page = pc & ~(LUMEN_TCG_PAGE_SIZE - 1);
    LumenQemuTcgMemoryRegion region = {0};
    const uint32_t access = LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_EXECUTE;
    const unsigned int jit_mode = qemu_thread_jit_callback_begin();
    const int queried = session->config.memory_query(session->config.opaque, page,
                                 LUMEN_TCG_PAGE_SIZE, access, &region);
    qemu_thread_jit_callback_end(jit_mode);
    if (!queried || !lumen_region_valid(&region, page, access)) {
        if (ir_cache_trace) qatomic_inc(&cache_key_unmapped);
        return false;
    }
    if (region.permissions & LUMEN_QEMU_TCG_WRITE) {
        if (ir_cache_trace) qatomic_inc(&cache_key_writable);
        return false;
    }
    if (region.reserved != LUMEN_QEMU_TCG_IMMUTABLE_CODE) {
        if (ir_cache_trace) qatomic_inc(&cache_key_unproven);
        return false;
    }
    key->pc = pc;
    key->cs_base = tb->cs_base;
    key->flags = tb->flags;
    key->cflags = tb_cflags(tb);
    key->max_insns = max_insns;
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(hash, (uint8_t *)region.host_base + (page - region.guest_base),
                      LUMEN_TCG_PAGE_SIZE);
    gsize length = sizeof(key->page_hash);
    g_checksum_get_digest(hash, key->page_hash, &length);
    g_checksum_free(hash);
    return true;
}

static bool lumen_ir_load(CPUState *cpu, TranslationBlock *tb, uint64_t pc,
                          void *host_pc, int max_insns)
{
    LumenQemuTcgSession *session = active_session;
    if (!session || session->cpu != cpu || max_insns <= 0 ||
        max_insns > TCG_MAX_INSNS || pc == session->config.return_target ||
        pc == LUMEN_TCG_SYSCALL_PC) return false;
    LumenQemuTcgCacheKey key = {
        .pc = pc, .cs_base = tb->cs_base, .flags = tb->flags,
        .cflags = tb_cflags(tb), .max_insns = max_insns
    };
    if (!LumenQemuCacheMayLoad(&key)) return false;
    if (!lumen_ir_key(cpu, tb, pc, max_insns, &key)) return false;
    GBytes *ir = LumenQemuCacheLoad(&key);
    if (!ir) return false;
    gsize size;
    const uint8_t *bytes = g_bytes_get_data(ir, &size);
    const bool hit = tcg_lumen_ir_import(tcg_ctx, tb, pc, max_insns, bytes, size);
    g_bytes_unref(ir);
    if (hit) LumenQemuCacheHit();
    else LumenQemuCacheReject();
    return hit;
}

static void lumen_ir_save(CPUState *cpu, const TranslationBlock *tb, uint64_t pc,
                          void *host_pc, int max_insns)
{
    if (!LumenQemuCacheMayStore() || !tb->size ||
        tb->size > LUMEN_TCG_PAGE_SIZE - (pc & (LUMEN_TCG_PAGE_SIZE - 1)) ||
        tb_page_addr1(tb) != -1) return;
    LumenQemuTcgCacheKey key;
    if (!lumen_ir_key(cpu, tb, pc, max_insns, &key)) return;
    GByteArray *ir = g_byte_array_new();
    if (tcg_lumen_ir_export(tcg_ctx, tb, pc, max_insns, ir))
        LumenQemuCacheStore(&key, ir->data, ir->len);
    else if (ir_cache_trace) qatomic_inc(&cache_export_unsupported);
    g_byte_array_unref(ir);
}

static LumenQemuTcgMap *lumen_memory_owner(MemoryRegion *memory)
{
    if (!memory) return NULL;
    Object *owner = memory_region_owner(memory);
    if (!owner || object_get_class(owner) != map_class) {
        return NULL;
    }
    LumenQemuTcgMap *map = (LumenQemuTcgMap *)owner;
    return memory == &map->memory ? map : NULL;
}

static bool lumen_ram_range_valid(const RAMBlock *block)
{
    return block && block->host && block->max_length &&
        block->max_length <= UINTPTR_MAX - (uintptr_t)block->host;
}

static bool lumen_code_ram_offset(CPUState *cpu, const CPUTLBEntryFull *full,
                                  vaddr address, void *host, uint64_t *offset)
{
    MemoryRegion *memory = full->section ? full->section->mr : NULL;
    LumenQemuTcgMap *map = lumen_memory_owner(memory);
    RAMBlock *block = map ? memory->ram_block : NULL;
    const uintptr_t pointer = (uintptr_t)host;
    (void)cpu;
    const uint64_t delta = map ? address - map->view.guest_base : UINT64_MAX;
    if (!map || !memory_region_is_ram(memory) ||
        address < map->view.guest_base || delta >= map->view.size ||
        (map->view.permissions & LUMEN_QEMU_TCG_EXECUTE) == 0 ||
        !lumen_ram_range_valid(block) || block->host != map->view.host_base ||
        block->mr != memory || delta >= block->max_length ||
        pointer != (uintptr_t)map->view.host_base + delta ||
        memory_region_get_ram_addr(memory) > UINT64_MAX - delta ||
        (ram_addr_t)(address + full->xlat_offset) !=
            memory_region_get_ram_addr(memory) + delta) {
        /* Deferred old RAMBlocks may still appear in the host/MRU lookup.
         * An owned page must use its exact TLB owner, never that fallback. */
        if (defer_reclaim && map) abort();
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        qatomic_inc(&code_ram_stats.fallback);
#endif
        return false;
    }
    /* The executable-page probe resolved this exact MemoryRegion. Its view,
     * owner and RAMBlock stay immutable under the CPU VM lease and RCU even
     * while other CPUs add disjoint topology. Use the same RAM address as the
     * soft-MMU's store/dirty/SMC path. A padded or aliased host lookup may pick
     * another block and is not an authority for this resolved code page. */
    *offset = memory_region_get_ram_addr(memory) + delta;
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    const uint64_t reference = address + full->xlat_offset;
    if (*offset != reference) {
        qatomic_inc(&code_ram_stats.mismatch);
        abort();
    }
    if (code_ram_force_reference) {
        *offset = reference;
        qatomic_inc(&code_ram_stats.reference);
    } else qatomic_inc(&code_ram_stats.fast);
    if (block->max_length > map->view.size) qatomic_inc(&code_ram_stats.padded);
    if (*offset != qemu_ram_addr_from_host_nofail(host)) qatomic_inc(&code_ram_stats.original_divergence);
#endif
    return true;
}

static bool lumen_ensure_map_locked(const LumenQemuTcgMemoryRegion *region)
{
    for (LumenQemuTcgMap *map = maps; map; map = map->next) {
        if (region->guest_base < map->view.guest_base + map->view.size &&
            map->view.guest_base < region->guest_base + region->size) {
            const bool matches = region->guest_base == map->view.guest_base &&
                region->size == map->view.size &&
                region->host_base == map->view.host_base &&
                region->permissions == map->view.permissions;
            return matches;
        }
    }
    LumenQemuTcgMap *map = (LumenQemuTcgMap *)object_new(lumen_map_type.name);
    assert(object_get_class(OBJECT(map)) == map_class);
    map->view = *region;
    memory_region_init_ram_ptr(&map->memory, OBJECT(map), "lumen-owned-guest-memory",
                               region->size, region->host_base);
    memory_region_add_subregion(get_system_memory(), region->guest_base, &map->memory);
    map->next = maps;
    maps = map;
    return true;
}

static bool lumen_ensure_page_map(LumenQemuTcgMemoryRegion *region, uint64_t page)
{
    /* Lookup, clipping and publication share the BQL with parallel TLB fills.
     * No separate topology mutex may be held while acquiring the BQL: queued
     * exclusive CPU work can release it while waiting for running CPUs. */
    bql_lock();
    uint64_t begin = region->guest_base;
    uint64_t end = begin + region->size;
    for (const LumenQemuTcgMap *map = maps; map; map = map->next) {
        if (region->guest_base >= map->view.guest_base + map->view.size ||
            map->view.guest_base >= region->guest_base + region->size) continue;
        if (page >= map->view.guest_base &&
            page - map->view.guest_base < map->view.size) {
            /* A fresh adjacent mapping can grow the provider's proven extent.
             * Reuse an existing complete view only when this page retains its
             * exact backing offset and full permission mask. Inconsistent or
             * narrowed proofs never replace a live view or widen its rights. */
            const uintptr_t queried = (uintptr_t)region->host_base + page - region->guest_base;
            const uintptr_t existing = (uintptr_t)map->view.host_base + page - map->view.guest_base;
            const bool matches = queried == existing && region->permissions == map->view.permissions;
            if (matches) *region = map->view;
            bql_unlock();
            return matches;
        }
        if (map->view.guest_base + map->view.size <= page)
            begin = MAX(begin, map->view.guest_base + map->view.size);
        else
            end = MIN(end, map->view.guest_base);
    }
    /* Choose the largest aligned dyadic subrange containing this complete
     * page, inside the positive proof and outside every live map. Keep the
     * provider's ordinary dyadic grouping even after a neighboring addition
     * grows a proof; no existing view is replaced or granted new rights. */
    assert(begin <= page && end - page >= LUMEN_TCG_PAGE_SIZE);
    uint64_t size = LUMEN_TCG_PAGE_SIZE;
    uint64_t base = page;
    while (size <= UINT64_MAX / 2) {
        const uint64_t next_size = size * 2;
        const uint64_t next_base = page & ~(next_size - 1);
        if (next_base < begin || next_size > end - next_base) break;
        base = next_base;
        size = next_size;
    }
    region->host_base = (void *)((uintptr_t)region->host_base + base - region->guest_base);
    region->guest_base = base;
    region->size = size;
    const bool mapped = lumen_ensure_map_locked(region);
    bql_unlock();
    return mapped;
}

#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
static bool lumen_ensure_map(const LumenQemuTcgMemoryRegion *region)
{
    bql_lock();
    const bool mapped = lumen_ensure_map_locked(region);
    bql_unlock();
    return mapped;
}

void LumenQemuTcgCodeRamTestReset(uint32_t force_reference)
{
    lumen_execution_enter(LUMEN_OP_VERIFY);
    memset(&code_ram_stats, 0, sizeof(code_ram_stats));
    code_ram_force_reference = force_reference != 0;
    lumen_execution_leave();
}

void LumenQemuTcgCodeRamTestRead(LumenQemuTcgCodeRamStats *stats)
{
    lumen_execution_enter(LUMEN_OP_VERIFY);
    *stats = code_ram_stats;
    lumen_execution_leave();
}

uint32_t LumenQemuTcgCodeRamTestWritableVictim(LumenQemuTcgSession *session,
                                             uint64_t address)
{
    if (!session || !session->open || session->executing ||
        !qemu_thread_is_self(&session->owner)) return 0;
    lumen_execution_enter(LUMEN_OP_VERIFY);
    const uint64_t page = address & ~(LUMEN_TCG_PAGE_SIZE - 1);
    int mmu = cpu_mmu_index(session->cpu, false);
    uint32_t found = 0;
    for (unsigned int i = 0; i < CPU_VTLB_SIZE; ++i) {
        uint64_t write = session->cpu->neg.tlb.d[mmu].vtable[i].addr_write;
        if ((write & ~(LUMEN_TCG_PAGE_SIZE - 1)) == page) found = 1;
    }
    lumen_execution_leave();
    return found;
}

uint32_t LumenQemuTcgCodeRamTestProbe(LumenQemuTcgSession *session,
    uint64_t address, const LumenQemuTcgMemoryRegion *add_after_warm,
    LumenQemuTcgCodeRamProbe *result)
{
    if (!session || !session->open || session->executing || !result ||
        !qemu_thread_is_self(&session->owner)) {
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    lumen_execution_enter(LUMEN_OP_VERIFY);
    lumen_jit_scope_begin();
    CPUState *cpu = session->cpu;
    active_session = session;
    current_cpu = cpu;
    bql_lock();
    process_queued_cpu_work(cpu);
    bql_unlock();
    rcu_read_lock();
    memset(result, 0, sizeof(*result));
    int mmu = cpu_mmu_index(cpu, true);
    const uint64_t page = address & ~(LUMEN_TCG_PAGE_SIZE - 1);
    for (unsigned int i = 0; i < CPU_VTLB_SIZE; ++i) {
        if (cpu->neg.tlb.d[mmu].vtable[i].addr_code == page) {
            result->victim_present = 1;
        }
    }
    void *host = NULL;
    CPUTLBEntryFull *full = NULL;
    uint32_t status = LUMEN_QEMU_TCG_READY;
    probe_access_full(cpu_env(cpu), address, 1, MMU_INST_FETCH, mmu,
                      true, &host, &full, 0);
    if (!host || !full || full->lg_page_size < 12) {
        status = LUMEN_QEMU_TCG_GUEST_FAULT;
    } else {
        /* Keep the original full and section alive through the topology
         * change. A normal commit may queue a TLB flush; this test therefore
         * checks a genuinely pre-existing entry before that queued flush. */
        CPUTLBEntryFull saved = *full;
        uint64_t offset;
        result->warm_owner_resolved = lumen_code_ram_offset(cpu, &saved, address,
                                                     host, &offset);
        if (add_after_warm && !lumen_ensure_map(add_after_warm)) {
            status = LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        } else {
            result->owner_resolved = lumen_code_ram_offset(cpu, &saved, address,
                                                      host, &offset);
            result->warm_rejected = add_after_warm && !result->owner_resolved;
            result->canonical_offset = address + saved.xlat_offset;
            result->original_offset = qemu_ram_addr_from_host_nofail(host);
            result->ram_offset = result->owner_resolved ? offset : result->original_offset;
            RAMBlock *block = saved.section->mr->ram_block;
            LumenQemuTcgMap *owner = lumen_memory_owner(saved.section->mr);
            result->block_length = block ? block->max_length : 0;
            result->view_length = owner ? owner->view.size : 0;
        }
    }
    rcu_read_unlock();
    active_session = NULL;
    lumen_jit_scope_end();
    lumen_execution_leave();
    return status;
}
#endif

static bool lumen_tlb_fill(CPUState *cpu, vaddr address, int size,
                           MMUAccessType type, int mmu_index,
                           bool probe, uintptr_t retaddr)
{
    LumenQemuTcgSession *session = active_session;
    const uint64_t page = address & ~(LUMEN_TCG_PAGE_SIZE - 1);
    const uint32_t access = type == MMU_DATA_STORE ? LUMEN_QEMU_TCG_WRITE :
        type == MMU_INST_FETCH ? LUMEN_QEMU_TCG_EXECUTE : LUMEN_QEMU_TCG_READ;
    LumenQemuTcgMemoryRegion region = {0};
    const unsigned int jit_mode = qemu_thread_jit_callback_begin();
    const int queried = session->config.memory_query(session->config.opaque, page,
                                      LUMEN_TCG_PAGE_SIZE, access, &region);
    qemu_thread_jit_callback_end(jit_mode);
    if (!queried ||
        !lumen_region_valid(&region, page, access) || !lumen_ensure_page_map(&region, page)) {
        if (probe) {
            return false;
        }
        session->fault_address = address;
        session->fault_exception = EXCP0E_PAGE;
        session->fault_access = access;
        session->boundary = LUMEN_QEMU_TCG_GUEST_FAULT;
        cpu->exception_index = EXCP_INTERRUPT;
        cpu_loop_exit_restore(cpu, retaddr);
    }
    tlb_set_page(cpu, page, page, (int)region.permissions, mmu_index,
                  LUMEN_TCG_PAGE_SIZE);
    return true;
}

const char *LumenQemuTcgReceipt(void)
{
    return qatomic_load_acquire(&initialized) ? "qemu-11.1.2;tcg-native;shared-ps4-ps5;guest-os=none;abi=1" :
                         "qemu-11.1.2;uninitialized";
}

uint32_t LumenQemuTcgReadCacheStats(LumenQemuTcgCacheStats *stats)
{
    if (!qatomic_load_acquire(&initialized) || !stats ||
        stats->struct_size != sizeof(*stats) || stats->abi_version != 1)
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    LumenQemuCacheStats(stats);
    return LUMEN_QEMU_TCG_READY;
}

static void lumen_cache_trace_summary(void)
{
    LumenQemuTcgCacheStats stats = {0};
    LumenQemuCacheStats(&stats);
    fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_STATS_V1 hits=%" PRIu64 " misses=%" PRIu64
        " stores=%" PRIu64 " rejected=%" PRIu64 " pretranslated=%" PRIu64
        " key_queries=%" PRIu64 " unmapped=%" PRIu64 " writable=%" PRIu64
        " unproven=%" PRIu64 " unsupported_ir=%" PRIu64 " diagnostic=1\n",
        stats.hits, stats.misses, stats.stores, stats.rejected, stats.pretranslated,
        qatomic_read(&cache_key_queries), qatomic_read(&cache_key_unmapped),
        qatomic_read(&cache_key_writable), qatomic_read(&cache_key_unproven),
        qatomic_read(&cache_export_unsupported));
}

uint32_t LumenQemuTcgInitialize(uint32_t requested_threads)
{
    if (qatomic_load_acquire(&initialized)) {
        return requested_threads <= max_threads ? LUMEN_QEMU_TCG_READY :
                                                   LUMEN_QEMU_TCG_CAPACITY;
    }
    if (!requested_threads || requested_threads > LUMEN_TCG_MAX_HOST_THREADS) {
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    /* Parse before initializing QEMU, whose initialization establishes the
     * monotonic clock. No clock or guest execution is needed to select policy.
     * Once initialized, the earlier return keeps this choice process-latched. */
    const char *chaining = getenv("LUMEN_QEMU_TCG_CHAINING");
    if (chaining == NULL || strcmp(chaining, "1") == 0) {
        block_chaining = true;
    } else if (strcmp(chaining, "0") == 0) {
        block_chaining = false;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_CHAINING_V1 enabled=0 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *parallel = getenv("LUMEN_QEMU_TCG_PARALLEL");
    if (parallel == NULL || strcmp(parallel, "1") == 0) {
        parallel_execution = true;
    } else if (strcmp(parallel, "0") == 0) {
        parallel_execution = false;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_PARALLEL_V1 enabled=0 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *scheduler = getenv("LUMEN_QEMU_TCG_SCHEDULER");
    if (scheduler == NULL || strcmp(scheduler, "fifo") == 0) {
        fair_budget_scheduler = false;
    } else if (strcmp(scheduler, "fair-budget") == 0) {
        fair_budget_scheduler = true;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_SCHEDULER_V1 policy=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *reclaim = getenv("LUMEN_QEMU_TCG_DEFER_RECLAIM");
    if (reclaim == NULL || strcmp(reclaim, "0") == 0) {
        defer_reclaim = false;
    } else if (strcmp(reclaim, "1") == 0) {
        defer_reclaim = true;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_RECLAIM_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *trace = getenv("LUMEN_QEMU_TCG_SCHEDULER_TRACE");
    if (trace == NULL || strcmp(trace, "0") == 0) {
        scheduler_trace = false;
    } else if (strcmp(trace, "1") == 0) {
        scheduler_trace = true;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_SCHED_TRACE_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *raw_clock = getenv("LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK");
    if (raw_clock == NULL || strcmp(raw_clock, "0") == 0) {
        raw_quantum_clock = false;
    } else if (strcmp(raw_clock, "1") == 0) {
#ifndef __APPLE__
        fprintf(stderr, "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK_V1 option=unsupported initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
#else
        const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        if (!now || now > INT64_MAX - LUMEN_TCG_QUANTUM_NS) {
            fprintf(stderr, "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK_V1 clock=failed initialization=denied\n");
            return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        }
        raw_quantum_clock = true;
#endif
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *coalescing = getenv("LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING");
    if (coalescing == NULL || strcmp(coalescing, "0") == 0) {
        tlb_commit_coalescing = false;
    } else if (strcmp(coalescing, "1") == 0) {
        tlb_commit_coalescing = true;
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *jit = getenv("LUMEN_QEMU_TCG_JIT_EXEC_CACHE");
    if (jit == NULL || strcmp(jit, "0") == 0) {
        jit_exec_cache = false;
    } else if (strcmp(jit, "1") == 0) {
#ifndef __APPLE__
        fprintf(stderr, "LUMEN_QEMU_TCG_JIT_CACHE_V1 enabled=0 option=unsupported initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
#else
        jit_exec_cache = true;
#endif
    } else {
        fprintf(stderr, "LUMEN_QEMU_TCG_JIT_CACHE_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *cache_option = getenv("LUMEN_QEMU_TCG_IR_CACHE");
    bool persistent_cache;
    if (cache_option == NULL || strcmp(cache_option, "1") == 0) persistent_cache = true;
    else if (strcmp(cache_option, "0") == 0) persistent_cache = false;
    else {
        fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_V1 option=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const char *cache_dir = persistent_cache ? getenv("LUMEN_QEMU_TCG_IR_CACHE_DIR") : NULL;
    const char *cache_trace = getenv("LUMEN_QEMU_TCG_IR_CACHE_TRACE");
    if (cache_trace == NULL || strcmp(cache_trace, "0") == 0) ir_cache_trace = false;
    else if (strcmp(cache_trace, "1") == 0) ir_cache_trace = true;
    else return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    /* Preserve explicit zero from older launch configurations. The excluded
     * pretranslation experiment cannot be enabled in this baseline. */
    const char *ahead = getenv("LUMEN_QEMU_TCG_PRETRANSLATE");
    if (ahead != NULL && strcmp(ahead, "0") != 0) {
        fprintf(stderr, "LUMEN_QEMU_TCG_PRETRANSLATE_V1 option=unsupported initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    char *default_cache_dir = NULL;
    if (persistent_cache && cache_dir == NULL) {
#ifdef __ANDROID__
        /* The app supplies Context.getCacheDir() before native initialization.
         * Android has no process HOME contract from which to guess app storage. */
        fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_V1 directory=missing-app-cache initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
#else
        const char *cache_home = g_get_user_cache_dir();
        if (!cache_home || cache_home[0] != '/' || g_mkdir_with_parents(cache_home, 0700) != 0) {
            fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_V1 directory=invalid-user-cache initialization=denied\n");
            return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        }
        default_cache_dir = g_build_filename(cache_home, "lumen-qemu-tcg-ir", NULL);
        cache_dir = default_cache_dir;
#endif
    }
    const bool cache_initialized = LumenQemuCacheInitialize(cache_dir, LUMEN_QEMU_IR_CACHE_BUILD_ID);
    g_free(default_cache_dir);
    if (!cache_initialized) {
        fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_V1 directory=invalid initialization=denied\n");
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    char smp[64];
    snprintf(smp, sizeof(smp), "1,maxcpus=%u", requested_threads);
    const char *arguments[] = {"lumen-qemu-tcg", "-machine", "lumen-tcg",
        "-accel", "tcg,thread=multi", "-smp", smp, "-S", "-display", "none",
        "-monitor", "none", "-serial", "none", NULL};
    char *argv[ARRAY_SIZE(arguments)];
    for (size_t i = 0; i < ARRAY_SIZE(arguments); ++i) {
        argv[i] = (char *)arguments[i];
    }
    qemu_mutex_init(&execution_mutex);
    qemu_init(ARRAY_SIZE(arguments) - 1, argv);
    map_class = object_class_by_name(lumen_map_type.name);
    assert(map_class);
    thread_has_rcu = true;
    accel_ops = *cpus_get_accel();
    accel_ops.create_vcpu_thread = lumen_create_vcpu;
    accel_ops.kick_vcpu_thread = lumen_kick_vcpu;
    accel_ops.get_elapsed_ticks = lumen_elapsed_ticks;
    cpus_register_accel(&accel_ops);
    /* The first CPU realization creates the target TCG globals using the
     * initializer's tcg_init_ctx TLS. Complete that step here before any
     * worker clones the context. This inactive CPU and its QOM ownership
     * live for the process; it never consumes a translation-thread slot. */
    bootstrap_realizing = true;
    Object *bootstrap = object_new(X86_CPU_TYPE_NAME("max"));
    object_property_set_int(bootstrap, "apic-id", next_apic_id++, &error_abort);
    qdev_realize(DEVICE(bootstrap), NULL, &error_abort);
    bootstrap_realizing = false;
    max_threads = requested_threads;
    qatomic_store_release(&initialized, 1);
    if (ir_cache_trace) atexit(lumen_cache_trace_summary);
    bql_unlock();
    replay_mutex_unlock();
    fprintf(stderr, "LUMEN_QEMU_TCG_SCHEDULER_V1 policy=%s max_quantum_ns=%" PRId64
            " min_quantum_ns=%" PRId64 " clock=%s fifo_order=preserved "
            "latency_bound=unqualified wake=targeted trace=%u\n", fair_budget_scheduler ? "fair-budget" : "fifo",
            LUMEN_TCG_QUANTUM_NS,
            fair_budget_scheduler ? LUMEN_TCG_MIN_QUANTUM_NS : LUMEN_TCG_QUANTUM_NS,
            raw_quantum_clock ? "darwin_monotonic_raw" : "existing_monotonic",
            scheduler_trace ? 1 : 0);
    fprintf(stderr, "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK_V1 enabled=%u default=0 "
            "host_quantum_only=1 guest_clock=unchanged trace_clock=unchanged "
            "clock=%s sleep_included=%s frequency_time_adjustments=%s boundary=every_executed_tb\n",
            raw_quantum_clock ? 1 : 0,
            raw_quantum_clock ? "darwin_monotonic_raw" : "existing_monotonic",
            raw_quantum_clock ? "1" : "existing_contract",
            raw_quantum_clock ? "excluded" : "existing_contract");
    fprintf(stderr, "LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING_V1 enabled=%u default=0 "
            "merge=tail_identical_no_data kick=every_commit order=preserved "
            "tlb=full rcu_fence=unchanged\n", tlb_commit_coalescing ? 1 : 0);
    fprintf(stderr, "LUMEN_QEMU_TCG_JIT_CACHE_V1 enabled=%u scope=core_operation "
            "callbacks=explicit_contract write_transitions=unconditional\n", jit_exec_cache ? 1 : 0);
    fprintf(stderr, "LUMEN_QEMU_TCG_PARALLEL_V1 enabled=%u scope=run "
            "exclusive=open-invalidate-verify admission=fifo default=1\n", parallel_execution ? 1 : 0);
    fprintf(stderr, "LUMEN_QEMU_TCG_CHAINING_V1 enabled=%u boundary=every_executed_tb "
            "hle=dynamic quantum=preserved default=1\n", block_chaining ? 1 : 0);
    fprintf(stderr, "LUMEN_QEMU_TCG_RECLAIM_V1 deferred=%u batch_limit=%u "
            "tb_tlb_flush=global topology=removed lifetime=rcu default=0\n",
            defer_reclaim ? 1 : 0, LUMEN_TCG_RECLAIM_BATCH_LIMIT);
    fprintf(stderr, "LUMEN_QEMU_TCG_IR_CACHE_V1 enabled=%u pretranslate=0 "
            "product=canonical_scalar_ir native_codegen=current jit=remaining "
            "build_id=%s default=on pretranslate=excluded\n", LumenQemuCacheEnabled(),
            LUMEN_QEMU_IR_CACHE_BUILD_ID);
    return LUMEN_QEMU_TCG_READY;
}

static void lumen_long_mode(CPUX86State *env)
{
    cpu_x86_update_cr0(env, CR0_PE_MASK | CR0_MP_MASK | CR0_ET_MASK | CR0_NE_MASK);
    uint32_t cr4 = CR4_OSFXSR_MASK | CR4_OSXSAVE_MASK;
    if (env->features[FEAT_7_0_EBX] & CPUID_7_0_EBX_FSGSBASE) {
        cr4 |= CR4_FSGSBASE_MASK;
    }
    cpu_x86_update_cr4(env, cr4);
    env->efer = MSR_EFER_LME | MSR_EFER_LMA | MSR_EFER_SCE;
    env->hflags |= HF_LMA_MASK;
    env->xcr0 = XSTATE_FP_MASK | XSTATE_SSE_MASK | XSTATE_YMM_MASK;
    env->star = UINT64_C(0x0013000800000000);
    env->lstar = LUMEN_TCG_SYSCALL_PC;
    env->fmask = 0;
    const unsigned int data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
                                    (3 << DESC_DPL_SHIFT) | DESC_B_MASK;
    const unsigned int code_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                                    DESC_R_MASK | (3 << DESC_DPL_SHIFT) | DESC_L_MASK;
    cpu_x86_load_seg_cache(env, R_CS, 0x1b, 0, UINT32_MAX, code_flags);
    cpu_x86_load_seg_cache(env, R_SS, 0x23, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_DS, 0x23, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_ES, 0x23, 0, UINT32_MAX, data_flags);
}

uint32_t LumenQemuTcgOpen(const LumenQemuTcgRunConfig *config,
                         LumenQemuTcgSession **output)
{
    const bool legacy = config && config->abi_version == 1 &&
        config->struct_size == offsetof(LumenQemuTcgRunConfig, flags);
    const bool current = config && config->abi_version == 2 && config->struct_size == sizeof(*config) &&
        (config->flags & ~LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT) == 0 && config->reserved == 0;
    if (!qatomic_load_acquire(&initialized) || !config || !output || (!legacy && !current) ||
        !config->memory_query || !config->return_target ||
        config->return_target == LUMEN_TCG_SYSCALL_PC) {
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (thread_depth >= LUMEN_TCG_MAX_NESTING) {
        return LUMEN_QEMU_TCG_CAPACITY;
    }
    lumen_execution_enter(LUMEN_OP_OPEN);
    if (!thread_registered && registered_threads >= max_threads) {
        lumen_execution_leave();
        return LUMEN_QEMU_TCG_CAPACITY;
    }
    lumen_jit_scope_begin();
    if (!thread_has_rcu) {
        rcu_register_thread();
        thread_has_rcu = true;
        thread_owns_rcu = true;
    }
    LumenQemuTcgSession *session = thread_sessions[thread_depth];
    if (!session) {
        session = g_new0(LumenQemuTcgSession, 1);
        bql_lock();
        Object *object = object_new(X86_CPU_TYPE_NAME("max"));
        object_property_set_int(object, "apic-id", next_apic_id++, &error_abort);
        object_property_set_int(object, "tsc-frequency", LumenQemuTcgTscFrequency(), &error_abort);
        object_property_set_int(object, "min-level", 0x15, &error_abort);
        qdev_realize(DEVICE(object), NULL, &error_abort);
        session->cpu = CPU(object);
        session->cpu_class = *session->cpu->cc;
        session->tcg_ops = *session->cpu->cc->tcg_ops;
        original_get_tb_state = session->tcg_ops.get_tb_cpu_state;
        session->tcg_ops.get_tb_cpu_state = lumen_get_tb_state;
        session->tcg_ops.tb_exec_boundary = lumen_tb_exec_boundary;
        session->tcg_ops.lumen_tlb_commit_coalescing = tlb_commit_coalescing;
        session->tcg_ops.do_interrupt = lumen_interrupt;
        session->tcg_ops.tlb_fill = lumen_tlb_fill;
        session->tcg_ops.code_ram_offset = lumen_code_ram_offset;
        if (LumenQemuCacheEnabled()) {
            session->tcg_ops.lumen_ir_load = lumen_ir_load;
            session->tcg_ops.lumen_ir_save = lumen_ir_save;
        }
        session->cpu_class.tcg_ops = &session->tcg_ops;
        session->cpu->cc = &session->cpu_class;
        bql_unlock();
        thread_sessions[thread_depth] = session;
    }
    bql_lock();
    cpu_reset(session->cpu);
    lumen_long_mode(cpu_env(session->cpu));
    x86_cpu_set_a20(X86_CPU(session->cpu), 1);
    bql_unlock();
    memset(&session->config, 0, sizeof(session->config));
    memcpy(&session->config, config, config->struct_size);
    session->open = true;
    session->executing = false;
    qemu_thread_get_self(&session->owner);
    qatomic_set(&session->interrupt_requested, 0);
    ++thread_depth;
    *output = session;
    lumen_jit_scope_end();
    lumen_execution_leave();
    return LUMEN_QEMU_TCG_READY;
}

uint32_t LumenQemuTcgRun(LumenQemuTcgSession *session,
                        LumenQemuTcgGuestState *state)
{
    if (!session || !session->open || session->executing || !state ||
        state->struct_size != sizeof(*state) || state->abi_version != 1 ||
        !qemu_thread_is_self(&session->owner) || !thread_depth ||
        thread_sessions[thread_depth - 1] != session) {
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    const uint64_t backlog = lumen_execution_enter(LUMEN_OP_RUN);
    lumen_jit_scope_begin();
    session->executing = true;
    active_session = session;
    CPUState *cpu = session->cpu;
    /* A nested session or exclusive work item can leave another context in
     * TLS. Queued exclusive work and atomic fallback require this Run's CPU
     * before tcg_cpu_exec gets its own chance to publish current_cpu. */
    current_cpu = cpu;
    CPUX86State *env = cpu_env(cpu);
    lumen_long_mode(env);
    memcpy(env->regs, state->gprs, sizeof(state->gprs));
    env->eip = state->rip;
    env->segs[R_FS].base = state->fs_base;
    env->segs[R_GS].base = state->gs_base;
    cpu_load_eflags(env, state->rflags, UINT32_MAX);
    cpu_set_mxcsr(env, state->mxcsr);
    env->fpuc = state->x87_control;
    update_fp_status(env);
    for (int i = 0; i < 16; ++i) {
        memcpy(&env->xmm_regs[i], state->ymm[i], sizeof(state->ymm[i]));
    }
    session->boundary = LUMEN_QEMU_TCG_READY;
    session->fault_exception = 0;
    session->fault_address = 0;
    session->fault_access = 0;
    cpu->halted = false;
    cpu->stopped = false;
    cpu->stop = false;
    cpu->interrupt_request = 0;
    session->deadline = lumen_run_clock() + lumen_run_quantum(backlog);
    int result;
    do {
        bql_lock();
        process_queued_cpu_work(cpu);
        bql_unlock();
        qatomic_set(&cpu->exit_request, 0);
        qatomic_set(&cpu->neg.icount_decr.u32, 0);
        if (qatomic_xchg(&session->interrupt_requested, 0)) {
            session->boundary = LUMEN_QEMU_TCG_INTERRUPTED;
            break;
        }
        result = tcg_cpu_exec(cpu);
        if (result == EXCP_ATOMIC) {
            current_cpu = cpu;
            cpu_exec_step_atomic(cpu);
        } else if (result != EXCP_INTERRUPT && !session->boundary) {
            session->fault_exception = result;
            session->boundary = LUMEN_QEMU_TCG_GUEST_FAULT;
        }
    } while (!session->boundary);
    state->rip = env->eip;
    memcpy(state->gprs, env->regs, sizeof(state->gprs));
    state->rflags = cpu_compute_eflags(env);
    state->fs_base = env->segs[R_FS].base;
    state->gs_base = env->segs[R_GS].base;
    state->mxcsr = env->mxcsr;
    state->x87_control = env->fpuc;
    for (int i = 0; i < 16; ++i) {
        memcpy(state->ymm[i], &env->xmm_regs[i], sizeof(state->ymm[i]));
    }
    state->boundary_target = env->eip;
    state->fault_exception = session->fault_exception;
    state->fault_address = session->fault_address;
    state->fault_access = session->fault_access;
    session->executing = false;
    active_session = NULL;
    cpu->stopped = true;
    lumen_jit_scope_end();
    lumen_execution_leave();
    return session->boundary;
}

void LumenQemuTcgClose(LumenQemuTcgSession *session)
{
    if (!session || !session->open || session->executing ||
        !qemu_thread_is_self(&session->owner) || !thread_depth ||
        thread_sessions[thread_depth - 1] != session) {
        return;
    }
    session->open = false;
    --thread_depth;
    if (!thread_depth && thread_owns_rcu) {
        rcu_unregister_thread();
        thread_has_rcu = false;
        thread_owns_rcu = false;
    }
}

void LumenQemuTcgInterrupt(LumenQemuTcgSession *session)
{
    if (session) {
        qatomic_set(&session->interrupt_requested, 1);
        cpu_exit(session->cpu);
    }
}

static bool lumen_reclaim_metadata_only(uint64_t *count, uint64_t *bytes)
{
    assert(bql_locked());
    *count = *bytes = 0;
    RAMBlockNotifier *notifier;
    QLIST_FOREACH(notifier, &ram_list.ramblock_notifiers, next) {
        if (notifier->ram_block_added || notifier->ram_block_removed) return false;
    }
    for (LumenQemuTcgMap *map = maps; map; map = map->next) {
        RAMBlock *block = map->memory.ram_block;
        if (lumen_memory_owner(&map->memory) != map ||
            !lumen_ram_range_valid(block) || block->mr != &map->memory ||
            block->host != map->view.host_base || block->flags != RAM_PREALLOC ||
            block->fd != -1 || block->guest_memfd != -1 || block->attributes ||
            !map->view.size || map->view.size > block->max_length ||
            *count == UINT64_MAX || block->max_length > UINT64_MAX - *bytes)
            return false;
        ++*count;
        *bytes += block->max_length;
    }
    /* This fixed Lumen machine has no devices, migration, or block IO guest
     * execution. Under QEMU 11.1.2, FlatView destruction only unrefs regions;
     * qemu_ram_free removes RAMList metadata and queues reclaim_ramblock.
     * RAM_PREALLOC reclamation never reads, writes or frees borrowed host
     * bytes. Excluding removal notifiers and attributes excludes callbacks
     * which could retain external host-buffer registrations or effects. */
    return true;
}

static void lumen_reclaim_fence(void)
{
    /* FlatView destruction may queue RAMBlock reclamation behind the first
     * fence. A second fence covers that child callback before resetting the
     * backlog estimate. Admission stays exclusive across both fences. */
    drain_call_rcu();
    drain_call_rcu();
    pending_reclaim_batches = 0;
    pending_reclaim_maps = pending_reclaim_bytes = 0;
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    ++reclaim_stats.fence_pairs;
#endif
}

#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
uint32_t LumenQemuTcgReclaimTestRcuHold(uint32_t begin)
{
    if (!qatomic_load_acquire(&initialized) || begin > 1 || thread_depth)
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    if (begin) {
        if (thread_has_rcu || reclaim_test_read_locked)
            return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        rcu_register_thread();
        thread_has_rcu = thread_owns_rcu = true;
        rcu_read_lock();
        reclaim_test_read_locked = true;
    } else {
        if (!reclaim_test_read_locked) return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        rcu_read_unlock();
        rcu_unregister_thread();
        reclaim_test_read_locked = thread_has_rcu = thread_owns_rcu = false;
    }
    return LUMEN_QEMU_TCG_READY;
}

void LumenQemuTcgReclaimTestDrain(void)
{
    lumen_execution_enter(LUMEN_OP_VERIFY);
    lumen_reclaim_fence();
    lumen_execution_leave();
}

void LumenQemuTcgReclaimTestRead(LumenQemuTcgReclaimStats *stats)
{
    if (!stats) return;
    lumen_execution_enter(LUMEN_OP_VERIFY);
    bql_lock();
    *stats = reclaim_stats;
    stats->pending_batches = pending_reclaim_batches;
    stats->batch_limit = LUMEN_TCG_RECLAIM_BATCH_LIMIT;
    stats->pending_maps = pending_reclaim_maps;
    stats->pending_bytes = pending_reclaim_bytes;
    bql_unlock();
    lumen_execution_leave();
}

static void lumen_reclaim_test_removed(RAMBlockNotifier *notifier, void *host,
                                       size_t size, size_t max_size)
{
    assert(bql_locked());
    ++reclaim_stats.notifier_removals;
    if (notifier != &reclaim_test_notifier || host != reclaim_test_host ||
        size != reclaim_test_size || max_size != reclaim_test_size ||
        *(volatile unsigned char *)host != 0x9a)
        ++reclaim_stats.notifier_failures;
}

uint32_t LumenQemuTcgReclaimTestNotifier(uint32_t enable, void *host, uint64_t size)
{
    if (!qatomic_load_acquire(&initialized) || enable > 1 ||
        (enable && (!host || !size || size > SIZE_MAX)))
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    lumen_execution_enter(LUMEN_OP_VERIFY);
    bql_lock();
    uint32_t result = LUMEN_QEMU_TCG_READY;
    if ((bool)enable == reclaim_test_notifier_registered) {
        result = LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    } else if (enable) {
        reclaim_test_host = host;
        reclaim_test_size = size;
        reclaim_test_notifier.ram_block_removed = lumen_reclaim_test_removed;
        ram_block_notifier_add(&reclaim_test_notifier);
        reclaim_test_notifier_registered = true;
    } else {
        ram_block_notifier_remove(&reclaim_test_notifier);
        reclaim_test_notifier_registered = false;
        reclaim_test_host = NULL;
        reclaim_test_size = 0;
    }
    bql_unlock();
    lumen_execution_leave();
    return result;
}

typedef struct LumenQueueCoalescingTest {
    uint32_t events[8], count;
    uint32_t w_payload, a_payload;
    bool overflow, nested_started, nested_queued;
} LumenQueueCoalescingTest;
static __thread LumenQueueCoalescingTest *queue_coalescing_test;

static void lumen_queue_test_record(uint32_t event)
{
    LumenQueueCoalescingTest *test = queue_coalescing_test;
    if (test->count >= ARRAY_SIZE(test->events)) test->overflow = true;
    else test->events[test->count++] = event;
}

static void lumen_queue_test_a(CPUState *cpu, run_on_cpu_data data)
{
    assert(bql_locked());
    if (data.host_ptr) {
        queue_coalescing_test->a_payload = *(const uint32_t *)data.host_ptr;
        lumen_queue_test_record(4);
    } else {
        lumen_queue_test_record(1);
    }
}

static void lumen_queue_test_w(CPUState *cpu, run_on_cpu_data data)
{
    assert(bql_locked() && data.host_ptr != NULL);
    queue_coalescing_test->w_payload = *(const uint32_t *)data.host_ptr;
    lumen_queue_test_record(2);
}

static void lumen_queue_test_nested(CPUState *cpu, run_on_cpu_data data)
{
    assert(bql_locked() && data.host_ptr == NULL);
    lumen_queue_test_record(3);
    if (!queue_coalescing_test->nested_started) {
        queue_coalescing_test->nested_started = true;
        queue_coalescing_test->nested_queued =
            lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_nested);
    }
}

/* Upstream's partial/elided counters use ctpop16, so a full requested flush
 * accounts for MIN(NB_MMU_MODES, 16) counter-visible indices whether fully
 * dirty, partly dirty, or clean. Weight full_flush_count the same way to
 * expose callbacks without adding counters to production queue/TLB code. */
static uint64_t lumen_queue_test_flushes(CPUState *cpu)
{
    return MIN(NB_MMU_MODES, 16) * (uint64_t)cpu->neg.tlb.c.full_flush_count +
        cpu->neg.tlb.c.part_flush_count + cpu->neg.tlb.c.elide_flush_count;
}

uint32_t LumenQemuTcgCodeRamTestQueueCoalescing(void)
{
    CPUState *cpu = current_cpu, *other;
    if (!qatomic_load_acquire(&initialized) || !cpu ||
        !qemu_cpu_is_self(cpu) || queue_coalescing_test)
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    lumen_execution_enter(LUMEN_OP_VERIFY);
    lumen_jit_scope_begin();
    const bool register_rcu = !thread_has_rcu;
    if (register_rcu) {
        rcu_register_thread();
        thread_has_rcu = true;
    }
    bql_lock();
    CPU_FOREACH(other) {
        current_cpu = other;
        process_queued_cpu_work(other);
    }
    current_cpu = cpu;
    LumenQueueCoalescingTest test = {0};
    queue_coalescing_test = &test;
    uint32_t result = LUMEN_QEMU_TCG_INTERNAL_ERROR;
    LumenQemuTcgMap *topology[3] = {0};
    uint8_t *bytes = NULL;
    const CPUClass *original_class = cpu->cc;
    CPUClass test_class = *original_class;
    TCGCPUOps test_ops = *original_class->tcg_ops;
    const uint64_t address = UINT64_C(0x10000000000), size = UINT64_C(65536);
    uint64_t topology_flush_delta = 0;
    uint32_t first_value = 0, last_value = 0;
    uint32_t w_payload = 0x5722, a_payload = 0x4111;
    /* A/A uses the real queue and executes exactly one effect callback. */
    if (!lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a) ||
        lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a)) goto done;
    process_queued_cpu_work(cpu);
    if (test.overflow || test.count != 1 || test.events[0] != 1) goto done;
    /* An ordinary W item preserves A/W/A order and permits a new A job. */
    test = (LumenQueueCoalescingTest){0};
    if (!lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a)) goto done;
    async_run_on_cpu(cpu, lumen_queue_test_w, RUN_ON_CPU_HOST_PTR(&w_payload));
    if (!lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a) ||
        lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a)) goto done;
    process_queued_cpu_work(cpu);
    if (test.overflow || test.count != 3 || test.events[0] != 1 ||
        test.events[1] != 2 || test.events[2] != 1 ||
        test.w_payload != w_payload) goto done;
    /* A matching function with non-NULL data is ordinary work, not a
     * compatible coalescing tail. Preserve both its payload and the new A. */
    test = (LumenQueueCoalescingTest){0};
    async_run_on_cpu(cpu, lumen_queue_test_a, RUN_ON_CPU_HOST_PTR(&a_payload));
    if (!lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a) ||
        lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_a)) goto done;
    process_queued_cpu_work(cpu);
    if (test.overflow || test.count != 2 || test.events[0] != 4 ||
        test.events[1] != 1 || test.a_payload != a_payload) goto done;
    /* The first callback has been dequeued but is still running when it
     * requests a successor. Its new work must execute independently. */
    test = (LumenQueueCoalescingTest){0};
    if (!lumen_async_run_on_cpu_coalesced(cpu, lumen_queue_test_nested)) goto done;
    process_queued_cpu_work(cpu);
    if (test.overflow || !test.nested_queued || test.count != 2 ||
        test.events[0] != 3 || test.events[1] != 3) goto done;

    /* Two genuine topology replacements occur before the next drain. MEM
     * and SMM listener jobs must merge, retire the warmed old TLB, and expose
     * the latest RAM bytes. Other CPUs retain their original selector. */
    for (LumenQemuTcgMap *map = maps; map; map = map->next) {
        if (address < map->view.guest_base + map->view.size &&
            map->view.guest_base < address + size) goto done;
    }
    bytes = qemu_try_memalign(size, 3 * size);
    if (!bytes) goto done;
    memset(bytes, 0, 3 * size);
    bytes[0] = 41; bytes[size] = 73; bytes[2 * size] = 99;
    for (unsigned int i = 0; i < ARRAY_SIZE(topology); ++i) {
        topology[i] = (LumenQemuTcgMap *)object_new(lumen_map_type.name);
        topology[i]->view = (LumenQemuTcgMemoryRegion){address, size,
            bytes + i * size, LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_WRITE, 0};
        memory_region_init_ram_ptr(&topology[i]->memory, OBJECT(topology[i]),
            "lumen-queue-coalescing-test", size, bytes + i * size);
    }
    test_ops.lumen_tlb_commit_coalescing = true;
    test_class.tcg_ops = &test_ops;
    cpu->cc = &test_class;
    memory_region_add_subregion(get_system_memory(), address, &topology[0]->memory);
    process_queued_cpu_work(cpu);
    const int mmu = cpu_mmu_index(cpu, false);
    rcu_read_lock();
    tlb_set_page(cpu, address, address,
        LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_WRITE, mmu, LUMEN_TCG_PAGE_SIZE);
    first_value = cpu_ldub_mmuidx_ra(cpu_env(cpu), address, mmu, 0);
    rcu_read_unlock();
    if (first_value != 41 || !(cpu->neg.tlb.c.dirty & (1U << mmu))) goto done;
    const uint64_t before = lumen_queue_test_flushes(cpu);
    for (unsigned int i = 0; i < 2; ++i) {
        memory_region_transaction_begin();
        memory_region_del_subregion(get_system_memory(), &topology[i]->memory);
        memory_region_add_subregion(get_system_memory(), address, &topology[i + 1]->memory);
        memory_region_transaction_commit();
    }
    process_queued_cpu_work(cpu);
    topology_flush_delta = lumen_queue_test_flushes(cpu) - before;
    if (topology_flush_delta != MIN(NB_MMU_MODES, 16) || cpu->neg.tlb.c.dirty) goto done;
    rcu_read_lock();
    tlb_set_page(cpu, address, address,
        LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_WRITE, mmu, LUMEN_TCG_PAGE_SIZE);
    last_value = cpu_ldub_mmuidx_ra(cpu_env(cpu), address, mmu, 0);
    rcu_read_unlock();
    if (last_value != 99) goto done;
    result = LUMEN_QEMU_TCG_READY;
done:
    memory_region_transaction_begin();
    for (unsigned int i = 0; i < ARRAY_SIZE(topology); ++i) {
        if (topology[i] && topology[i]->memory.container)
            memory_region_del_subregion(get_system_memory(), &topology[i]->memory);
    }
    memory_region_transaction_commit();
    CPU_FOREACH(other) {
        current_cpu = other;
        process_queued_cpu_work(other);
    }
    current_cpu = cpu;
    cpu->cc = original_class;
    queue_coalescing_test = NULL;
    for (unsigned int i = 0; i < ARRAY_SIZE(topology); ++i)
        if (topology[i]) object_unref(OBJECT(topology[i]));
    bql_unlock();
    /* Old FlatViews and their nested RAMBlock retirements may outlive the
     * positive guest view. Complete both RCU generations before byte reuse. */
    drain_call_rcu();
    drain_call_rcu();
    qemu_vfree(bytes);
    if (register_rcu) {
        rcu_unregister_thread();
        thread_has_rcu = false;
    }
    if (result == LUMEN_QEMU_TCG_READY)
        printf("Queue coalescing: A/A=1 A/W/A=ordered payloads=preserved nonnull_tail=distinct inflight/new=2 topology_replacements=2 full_flushes=1 sentinel=%u->%u\n",
               first_value, last_value);
    else
        fprintf(stderr, "Queue coalescing verification failed: events=%u overflow=%u nested=%u topology_flush_indices=%" PRIu64 " sentinel=%u->%u\n",
                test.count, test.overflow, test.nested_queued,
                topology_flush_delta, first_value, last_value);
    lumen_jit_scope_end();
    lumen_execution_leave();
    return result;
}
#endif

static uint32_t lumen_invalidate(uint64_t address, uint64_t size, bool additive)
{
    if (!qatomic_load_acquire(&initialized) || !size || address > UINT64_MAX - size) {
        return LUMEN_QEMU_TCG_INVALID_ARGUMENT;
    }
    lumen_execution_enter(LUMEN_OP_INVALIDATE);
    lumen_jit_scope_begin();
    /* A translated guest block requires an admitted memory mapping. With no
       mappings there is no live TB/TLB entry to retire. Earlier metadata may
       still be retained by RCU, independently of guest backing lifetime. */
    if (!maps) {
        lumen_jit_scope_end();
        lumen_execution_leave();
        return LUMEN_QEMU_TCG_READY;
    }
    if (additive) {
        bool intersects = false;
        /* Exclusive FIFO admission has drained all Run readers and excludes
         * Open/Invalidate/topology effects. Every positive TLB and live TB
         * requires one of these complete, permission-proved RAM views. There
         * are no cached negative mappings. Check the entire published range;
         * an uncertain/intersecting view retains the ordinary global flush. */
        for (const LumenQemuTcgMap *map = maps; map; map = map->next) {
            if (address < map->view.guest_base + map->view.size &&
                map->view.guest_base < address + size) {
                intersects = true;
                break;
            }
        }
        if (!intersects) {
            lumen_jit_scope_end();
            lumen_execution_leave();
            return LUMEN_QEMU_TCG_READY;
        }
    }
    CPUState *saved_current_cpu = current_cpu;
    bql_lock();
    uint64_t retired_count = 0, retired_bytes = 0;
    const bool metadata_only = defer_reclaim &&
        lumen_reclaim_metadata_only(&retired_count, &retired_bytes) &&
        retired_count <= LUMEN_TCG_RECLAIM_MAP_LIMIT &&
        retired_bytes <= LUMEN_TCG_RECLAIM_BYTE_LIMIT;
    tb_flush__exclusive_or_serial();
    CPUState *cpu;
    /* Publish one topology change while every CPU is quiescent. Keep our
       map references until commit; old FlatViews retain their own references
       until RCU reclamation, synchronously or under the metadata-only proof. */
    LumenQemuTcgMap *retired_maps = maps;
    memory_region_transaction_begin();
    for (LumenQemuTcgMap *map = retired_maps; map; map = map->next) {
        memory_region_del_subregion(get_system_memory(), &map->memory);
    }
    maps = NULL;
    /* Removing these enabled owned views from enabled system_memory queues
     * TLB flushes through every CPU address space's TCG commit listener.
     * Drain that work below before reclaiming backing or reopening admission;
     * a manual pre-commit flush would duplicate the listener's work. */
    memory_region_transaction_commit();
    while (retired_maps) {
        LumenQemuTcgMap *map = retired_maps;
        retired_maps = map->next;
        object_unref(OBJECT(map));
    }
    CPU_FOREACH(cpu) {
        current_cpu = cpu;
        process_queued_cpu_work(cpu);
    }
    current_cpu = saved_current_cpu;
    bql_unlock();
    if (metadata_only) {
        ++pending_reclaim_batches;
        /* Individual batches passed the limits above. Addition cannot
         * overflow: the prior returned backlog is below the same limits. */
        pending_reclaim_maps += retired_count;
        pending_reclaim_bytes += retired_bytes;
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        ++reclaim_stats.deferred_batches;
        reclaim_stats.max_pending_batches = MAX(reclaim_stats.max_pending_batches,
                                               pending_reclaim_batches);
#endif
        if (pending_reclaim_batches >= LUMEN_TCG_RECLAIM_BATCH_LIMIT ||
            pending_reclaim_maps >= LUMEN_TCG_RECLAIM_MAP_LIMIT ||
            pending_reclaim_bytes >= LUMEN_TCG_RECLAIM_BYTE_LIMIT)
            lumen_reclaim_fence();
    } else if (defer_reclaim) {
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        ++reclaim_stats.guard_fallbacks;
#endif
        lumen_reclaim_fence();
    } else {
        drain_call_rcu();
    }
    lumen_jit_scope_end();
    lumen_execution_leave();
    return LUMEN_QEMU_TCG_READY;
}

uint32_t LumenQemuTcgInvalidate(uint64_t address, uint64_t size)
{
    return lumen_invalidate(address, size, false);
}

uint32_t LumenQemuTcgInvalidateAdditive(uint64_t address, uint64_t size)
{
    return lumen_invalidate(address, size, true);
}
