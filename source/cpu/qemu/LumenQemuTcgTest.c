/* SPDX-License-Identifier: GPL-2.0-only */
#include "LumenQemuTcg.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
#include "LumenQemuTcgCodeRamTest.h"
#endif

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)
#define RETURN_PC UINT64_C(0x00007fffffff0000)

static bool option_enabled(const char *name)
{
    /* CHAINING and PARALLEL share the adopted default: a fresh process with
     * neither override must prove linked boundaries and real guest overlap.
     * Explicit 0 continues to exercise the rollback/serial negative control. */
    const char *value = getenv(name);
    return value == NULL || strcmp(value, "1") == 0;
}

typedef struct Fixture {
    unsigned char *bytes;
    uint64_t denied_page;
    uint32_t denied_access;
    uint64_t host_pc;
    _Atomic(LumenQemuTcgSession *) worker_session;
    _Atomic uint32_t worker_result;
    const LumenQemuTcgMemoryRegion *regions;
    uint32_t region_count;
    uint32_t query_count;
    bool whole_region;
    unsigned char *collision;
    bool disturb_host_jit, disturb_memory_jit, memory_jit_execute;
    uint32_t jit_host_changes, jit_memory_changes;
} Fixture;

static int memory_query(void *opaque, uint64_t address, uint64_t size,
                        uint32_t access, LumenQemuTcgMemoryRegion *region)
{
    Fixture *fixture = opaque;
#ifdef __APPLE__
    if (fixture->disturb_memory_jit) {
        pthread_jit_write_protect_np(fixture->memory_jit_execute &&
                                     access == LUMEN_QEMU_TCG_EXECUTE);
        ++fixture->jit_memory_changes;
    }
#endif
    if (fixture->collision && address == UINT64_C(0x100001000)) {
        *region = (LumenQemuTcgMemoryRegion){address, 4096, fixture->collision, 7, 0};
        return 1;
    }
    if (fixture->regions) {
        ++fixture->query_count;
        for (uint32_t i = 0; i < fixture->region_count; ++i) {
            const LumenQemuTcgMemoryRegion *view = &fixture->regions[i];
            if (address >= view->guest_base && address - view->guest_base < view->size &&
                size <= view->size - (address - view->guest_base) &&
                (view->permissions & access) == access) {
                *region = *view;
                return 1;
            }
        }
        return 0;
    }
    if (address >= 65536 || size > 65536 - address ||
        (address == fixture->denied_page && (access & fixture->denied_access))) {
        return 0;
    }
    region->guest_base = fixture->whole_region ? 0 : address;
    region->size = fixture->whole_region ? 65536 : 4096;
    region->host_base = fixture->bytes + region->guest_base;
    region->permissions = 7;
    region->reserved = 0;
    return 1;
}

static uint64_t native_host_function(uint64_t value) { return value + 57; }

static int host_query(void *opaque, uint64_t pc)
{
    Fixture *fixture = opaque;
#ifdef __APPLE__
    if (fixture->disturb_host_jit) {
        pthread_jit_write_protect_np(0);
        ++fixture->jit_host_changes;
    }
#endif
    return pc == fixture->host_pc;
}

static LumenQemuTcgRunConfig config_for(Fixture *fixture)
{
    LumenQemuTcgRunConfig config = {
        sizeof(config), 2, RETURN_PC, memory_query, host_query, fixture,
        LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT, 0
    };
    return config;
}

static LumenQemuTcgGuestState state_for(Fixture *fixture)
{
    LumenQemuTcgGuestState state = {0};
    state.struct_size = sizeof(state);
    state.abi_version = 1;
    state.rip = 4096;
    state.gprs[4] = 0xe000;
    state.rflags = 0x202;
    state.mxcsr = 0x1f80;
    state.x87_control = 0x37f;
    uint64_t return_pc = RETURN_PC;
    memcpy(fixture->bytes + state.gprs[4], &return_pc, sizeof(return_pc));
    return state;
}

/* Ordinary boundaries may span multiple scheduling quanta. Keep the
 * architectural state returned by each Run; a quantum is not completion. */
static uint32_t run_to_boundary(LumenQemuTcgSession *session,
                                LumenQemuTcgGuestState *state)
{
    uint32_t result;
    do {
        result = LumenQemuTcgRun(session, state);
    } while (result == LUMEN_QEMU_TCG_INTERRUPTED);
    return result;
}

static void *interrupt_worker(void *opaque)
{
    Fixture *fixture = opaque;
    LumenQemuTcgRunConfig config = config_for(fixture);
    LumenQemuTcgSession *session = NULL;
    uint32_t result = LumenQemuTcgOpen(&config, &session);
    if (result == LUMEN_QEMU_TCG_READY) {
        LumenQemuTcgGuestState state = state_for(fixture);
        atomic_store(&fixture->worker_session, session);
        result = LumenQemuTcgRun(session, &state);
        LumenQemuTcgClose(session);
    }
    atomic_store(&fixture->worker_result, result);
    return NULL;
}

static void *first_open_worker(void *opaque)
{
    Fixture *fixture = opaque;
    LumenQemuTcgRunConfig config = config_for(fixture);
    LumenQemuTcgSession *session = NULL;
    uint32_t result = LumenQemuTcgOpen(&config, &session);
    if (result == LUMEN_QEMU_TCG_READY) {
        LumenQemuTcgGuestState state = state_for(fixture);
        result = run_to_boundary(session, &state);
        if (result == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] != 42) {
            result = LUMEN_QEMU_TCG_GUEST_FAULT;
        }
        LumenQemuTcgClose(session);
    }
    atomic_store(&fixture->worker_result, result);
    return NULL;
}

typedef struct PersistentCpu {
    Fixture *fixture;
    uint64_t stack;
    _Atomic uint64_t pc;
    _Atomic uint32_t command;
    _Atomic uint32_t completed;
    _Atomic uint32_t result;
    _Atomic uint64_t value;
    _Atomic uint64_t fault_address;
    _Atomic uint32_t fault_access;
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    _Atomic uint32_t writable_victim;
#endif
} PersistentCpu;

static void *persistent_cpu_worker(void *opaque)
{
    PersistentCpu *worker = opaque;
    LumenQemuTcgRunConfig config = config_for(worker->fixture);
    LumenQemuTcgSession *session = NULL;
    uint32_t result = LumenQemuTcgOpen(&config, &session);
    atomic_store(&worker->result, result);
    atomic_store(&worker->completed, 1);
    if (result != LUMEN_QEMU_TCG_READY) return NULL;
    const struct timespec delay = {0, 1000000};
    for (;;) {
        const uint32_t command = atomic_load(&worker->command);
        if (command == UINT32_MAX) break;
        if (command <= atomic_load(&worker->completed)) {
            nanosleep(&delay, NULL);
            continue;
        }
        LumenQemuTcgGuestState state = {0};
        state.struct_size = sizeof(state);
        state.abi_version = 1;
        state.rip = atomic_load(&worker->pc);
        state.gprs[4] = worker->stack;
        state.rflags = 0x202;
        state.mxcsr = 0x1f80;
        state.x87_control = 0x37f;
        result = run_to_boundary(session, &state);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        if (worker->fixture->collision && atomic_load(&worker->pc) == 0x4000) {
            atomic_store(&worker->writable_victim,
                LumenQemuTcgCodeRamTestWritableVictim(session, 0x1100));
        }
#endif
        atomic_store(&worker->value, state.gprs[0]);
        atomic_store(&worker->fault_address, state.fault_address);
        atomic_store(&worker->fault_access, state.fault_access);
        atomic_store(&worker->result, result);
        atomic_store(&worker->completed, command);
    }
    LumenQemuTcgClose(session);
    return NULL;
}

static bool wait_cpu(PersistentCpu *worker, uint32_t command)
{
    const struct timespec delay = {0, 1000000};
    for (unsigned int i = 0; i < 3000; ++i) {
        if (atomic_load(&worker->completed) == command) return true;
        nanosleep(&delay, NULL);
    }
    return false;
}

static void command_cpu(PersistentCpu *worker, uint32_t command, uint64_t pc)
{
    atomic_store(&worker->pc, pc);
    atomic_store(&worker->command, command);
}

static int multi_cpu_invalidation(Fixture *fixture)
{
    /* Fill fifteen independent RAM maps and a writable target TLB on two
       live CPU sessions before either CPU translates the target as code. */
    const unsigned char warm[] = {
        0x48, 0xc7, 0xc0, 0, 0x10, 0, 0, /* mov rax, 0x1000 */
        0xb9, 15, 0, 0, 0,               /* mov ecx, 15 */
        0x48, 0x8b, 0x10,                /* mov rdx, [rax] */
        0x48, 0x05, 0, 0x10, 0, 0,       /* add rax, 0x1000 */
        0xe2, 0xf5, 0xc3                 /* loop; ret */
    };
    const unsigned char target[] = {0xb8, 42, 0, 0, 0, 0xc3};
    const unsigned char patch[] = {
        0xc6, 0x04, 0x25, 0x01, 0x11, 0, 0, 43, /* mov byte [0x1101], 43 */
        0xc3
    };
    memcpy(fixture->bytes + 0x4000, warm, sizeof(warm));
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    if (fixture->whole_region) {
        CHECK(posix_memalign((void **)&fixture->collision, 16384, 16384) == 0);
        memset(fixture->collision, 0, 16384);
        unsigned char collide[] = {
            0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
            0x48, 0x8b, 0x10, 0xc3
        };
        const uint64_t address = UINT64_C(0x100001000);
        memcpy(collide + 2, &address, sizeof(address));
        memcpy(fixture->bytes + 0x4000 + sizeof(warm) - 1, collide, sizeof(collide));
    }
#endif
    memcpy(fixture->bytes + 0x1100, target, sizeof(target));
    memcpy(fixture->bytes + 0x5000, patch, sizeof(patch));
    fixture->host_pc = (uint64_t)(uintptr_t)&native_host_function;
    PersistentCpu cpus[2] = {{.fixture = fixture, .stack = 0xa000},
                             {.fixture = fixture, .stack = 0xc000}};
    pthread_t threads[2];
    const uint64_t return_pc = RETURN_PC;
    for (unsigned int i = 0; i < 2; ++i)
        memcpy(fixture->bytes + cpus[i].stack, &return_pc, sizeof(return_pc));
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(pthread_create(&threads[i], NULL, persistent_cpu_worker, &cpus[i]) == 0);
        CHECK(wait_cpu(&cpus[i], 1));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_READY);
        command_cpu(&cpus[i], 2, 0x4000);
    }
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(wait_cpu(&cpus[i], 2));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) ==
              (fixture->collision ? UINT64_C(0x100001000) : 65536));
    }
    /* All maps now exist. A second warm pass consumes each CPU's queued
       topology flush before installing the writable target in its victim. */
    for (unsigned int i = 0; i < 2; ++i) command_cpu(&cpus[i], 3, 0x4000);
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(wait_cpu(&cpus[i], 3));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        if (fixture->collision) CHECK(atomic_load(&cpus[i].writable_victim));
#endif
    }
    command_cpu(&cpus[0], 4, 0x1100);
    CHECK(wait_cpu(&cpus[0], 4));
    CHECK(atomic_load(&cpus[0].result) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(atomic_load(&cpus[0].value) == 42);
    /* No host invalidation: guest SMC on the peer's already warm writable
       TLB must invalidate the reader's cached target TB. */
    command_cpu(&cpus[1], 4, 0x5000);
    CHECK(wait_cpu(&cpus[1], 4));
    CHECK(atomic_load(&cpus[1].result) == LUMEN_QEMU_TCG_RETURNED);
    for (unsigned int i = 0; i < 2; ++i) command_cpu(&cpus[i], 5, 0x1100);
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(wait_cpu(&cpus[i], 5));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) == 43);
    }

    /* Same guest VA, different owned backing; both open sessions keep their
       CPU state. The old backing is released only after the invalidator's
       transaction, queued CPU work and RCU drain have completed. */
    unsigned char *replacement = NULL;
    CHECK(posix_memalign((void **)&replacement, 4096, 65536) == 0);
    memcpy(replacement, fixture->bytes, 65536);
    replacement[0x1101] = 44;
    unsigned char *retired = fixture->bytes;
    fixture->bytes = replacement;
    /* Even an additive caller must fall back when the published range
       intersects a previously admitted view. */
    CHECK(LumenQemuTcgInvalidateAdditive(0x1100, 1) == LUMEN_QEMU_TCG_READY);
    free(retired);
    for (unsigned int i = 0; i < 2; ++i) command_cpu(&cpus[i], 6, 0x1100);
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(wait_cpu(&cpus[i], 6));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) == 44);
    }
    fixture->denied_page = 0x1000;
    fixture->denied_access = LUMEN_QEMU_TCG_EXECUTE;
    CHECK(LumenQemuTcgInvalidateAdditive(0x1000, 4096) == LUMEN_QEMU_TCG_READY);
    for (unsigned int i = 0; i < 2; ++i) command_cpu(&cpus[i], 7, 0x1100);
    for (unsigned int i = 0; i < 2; ++i) {
        CHECK(wait_cpu(&cpus[i], 7));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_GUEST_FAULT);
        CHECK(atomic_load(&cpus[i].fault_address) == 0x1100);
        CHECK(atomic_load(&cpus[i].fault_access) == LUMEN_QEMU_TCG_EXECUTE);
        atomic_store(&cpus[i].command, UINT32_MAX);
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    fixture->denied_access = 0;
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    free(fixture->collision);
    fixture->collision = NULL;
    return 0;
}

static int additive_map_retention(Fixture *storage)
{
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    CHECK(LumenQemuTcgInvalidateAdditive(0x5000, 0) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    CHECK(LumenQemuTcgInvalidateAdditive(UINT64_MAX - 3, 8) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    const unsigned char read_old[] = {0x8b, 0x04, 0x25, 0, 0x40, 0, 0, 0xc3};
    const unsigned char read_added[] = {0x8b, 0x04, 0x25, 0, 0x50, 0, 0, 0xc3};
    const unsigned char read_six[] = {0x8b, 0x04, 0x25, 0, 0x60, 0, 0, 0xc3};
    const unsigned char read_seven[] = {0x8b, 0x04, 0x25, 0, 0x70, 0, 0, 0xc3};
    memcpy(storage->bytes + 0x1000, read_old, sizeof(read_old));
    memcpy(storage->bytes + 0x1100, read_added, sizeof(read_added));
    memcpy(storage->bytes + 0x1200, read_six, sizeof(read_six));
    memcpy(storage->bytes + 0x1300, read_seven, sizeof(read_seven));
    const uint32_t old_value = 42, new_value = 73;
    memcpy(storage->bytes + 0x4000, &old_value, sizeof(old_value));
    memcpy(storage->bytes + 0x5000, &new_value, sizeof(new_value));
    memcpy(storage->bytes + 0x6000, &new_value, sizeof(new_value));
    memcpy(storage->bytes + 0x7000, &new_value, sizeof(new_value));
    LumenQemuTcgMemoryRegion regions[] = {
        {0x1000, 4096, storage->bytes + 0x1000, 5, 0},
        {0x4000, 4096, storage->bytes + 0x4000, 3, 0},
        {0xa000, 4096, storage->bytes + 0xa000, 3, 0},
        {0xc000, 4096, storage->bytes + 0xc000, 3, 0},
        {0x5000, 4096, storage->bytes + 0x5000, 3, 0},
        {0x6000, 4096, storage->bytes + 0x6000, 3, 0},
        {0x7000, 4096, storage->bytes + 0x7000, 3, 0},
    };
    Fixture fixture = {.bytes = storage->bytes, .regions = regions, .region_count = 4};
    PersistentCpu cpus[2] = {{.fixture = &fixture, .stack = 0xa000},
                             {.fixture = &fixture, .stack = 0xc000}};
    pthread_t threads[2];
    const uint64_t return_pc = RETURN_PC;
    for (unsigned int i = 0; i < 2; ++i) {
        memcpy(storage->bytes + cpus[i].stack, &return_pc, sizeof(return_pc));
        CHECK(pthread_create(&threads[i], NULL, persistent_cpu_worker, &cpus[i]) == 0);
        CHECK(wait_cpu(&cpus[i], 1));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_READY);
    }
    /* Fill all maps, then consume topology work and warm code/data/stack
       entries on both persistent CPUs. Commands run serially so query_count
       is an exact witness, independent of the parallel admission policy. */
    for (uint32_t command = 2; command <= 4; ++command) {
        for (unsigned int i = 0; i < 2; ++i) {
            command_cpu(&cpus[i], command, 0x1000);
            CHECK(wait_cpu(&cpus[i], command));
            CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
            CHECK(atomic_load(&cpus[i].value) == old_value);
        }
    }
    const uint32_t warm_queries = fixture.query_count;
    fixture.region_count = 5;
    /* Uniform rights now allow a larger proof covering the old and new
       adjacent pages. A retained old map must not reject that valid growth. */
    regions[1].size = 8192;
    CHECK(LumenQemuTcgInvalidateAdditive(0x5000, 4096) == LUMEN_QEMU_TCG_READY);
    for (unsigned int i = 0; i < 2; ++i) {
        command_cpu(&cpus[i], 5, 0x1000);
        CHECK(wait_cpu(&cpus[i], 5));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) == old_value);
        CHECK(fixture.query_count == warm_queries);
    }
    /* The newly published page must remain discoverable; retained old TLBs
       cannot act as a cached denial for a view that did not exist before. */
    for (unsigned int i = 0; i < 2; ++i) {
        command_cpu(&cpus[i], 6, 0x1100);
        CHECK(wait_cpu(&cpus[i], 6));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) == new_value);
    }
    CHECK(fixture.query_count > warm_queries);
    /* A fresh map queues a peer TLB flush. Contradictory callback witnesses
       must then reject containing-map reuse rather than silently replace
       backing or alter a live map's complete rights. */
    fixture.region_count = 6;
    CHECK(LumenQemuTcgInvalidateAdditive(0x6000, 4096) == LUMEN_QEMU_TCG_READY);
    command_cpu(&cpus[1], 7, 0x1200);
    CHECK(wait_cpu(&cpus[1], 7));
    CHECK(atomic_load(&cpus[1].result) == LUMEN_QEMU_TCG_RETURNED);
    regions[1].host_base = storage->bytes + 0x8000;
    command_cpu(&cpus[0], 7, 0x1000);
    CHECK(wait_cpu(&cpus[0], 7));
    CHECK(atomic_load(&cpus[0].result) == LUMEN_QEMU_TCG_GUEST_FAULT);
    CHECK(atomic_load(&cpus[0].fault_address) == 0x4000);
    regions[1].host_base = storage->bytes + 0x4000;
    command_cpu(&cpus[0], 8, 0x1000);
    CHECK(wait_cpu(&cpus[0], 8));
    CHECK(atomic_load(&cpus[0].result) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(atomic_load(&cpus[0].value) == old_value);
    fixture.region_count = 7;
    CHECK(LumenQemuTcgInvalidateAdditive(0x7000, 4096) == LUMEN_QEMU_TCG_READY);
    command_cpu(&cpus[1], 8, 0x1300);
    CHECK(wait_cpu(&cpus[1], 8));
    CHECK(atomic_load(&cpus[1].result) == LUMEN_QEMU_TCG_RETURNED);
    regions[1].permissions = LUMEN_QEMU_TCG_READ;
    command_cpu(&cpus[0], 9, 0x1000);
    CHECK(wait_cpu(&cpus[0], 9));
    CHECK(atomic_load(&cpus[0].result) == LUMEN_QEMU_TCG_GUEST_FAULT);
    CHECK(atomic_load(&cpus[0].fault_address) == 0x4000);
    regions[1].permissions = LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_WRITE;
    /* One publication can span holes and an existing code map. The whole
       range must trigger the global fallback, not only its first page. */
    storage->bytes[0x1004] = 0x50;
    CHECK(LumenQemuTcgInvalidateAdditive(0, 8192) == LUMEN_QEMU_TCG_READY);
    const uint32_t before_fallback_queries = fixture.query_count;
    for (unsigned int i = 0; i < 2; ++i) {
        command_cpu(&cpus[i], 10, 0x1000);
        CHECK(wait_cpu(&cpus[i], 10));
        CHECK(atomic_load(&cpus[i].result) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(atomic_load(&cpus[i].value) == new_value);
        CHECK(fixture.query_count > before_fallback_queries);
        atomic_store(&cpus[i].command, UINT32_MAX);
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    printf("Additive VM retention: two warm CPU TLBs unchanged, growing proof accepts fresh RAM, conflicting backing/rights denied, intersection fallback refreshed code\n");
    return 0;
}

static int additive_padding_smc(void)
{
    const uint32_t sizes[] = {4096, 8192, 12288, 20480, 28672, 32768};
    const unsigned int shapes = sizeof(sizes) / sizeof(sizes[0]);
    for (unsigned int shape = 0; shape < shapes * 2; ++shape) {
        CHECK(LumenQemuTcgInvalidate(0, UINT64_MAX) == LUMEN_QEMU_TCG_READY);
        unsigned char *storage = NULL;
        CHECK(posix_memalign((void **)&storage, 16384, 131072) == 0);
        memset(storage, 0, 131072);
        const uint32_t fresh_size = sizes[shape % shapes];
        const bool fresh_exec = shape >= shapes;
        /* Logical native ranges never alias; only RAMBlock host-page padding
           covers the older code. EXEC cases also make the new block a host
           lookup MRU before a cold peer translates that older code. */
        unsigned char *code = storage + fresh_size;
        const unsigned char target[] = {0xb8, 42, 0, 0, 0, 0xc3};
        const unsigned char patch[] = {0xc6, 0x04, 0x25, 0x01, 0x10, 0, 0, 43, 0xc3};
        const unsigned char read_first[] = {0x8b, 0x04, 0x25, 0, 0, 1, 0, 0xc3};
        unsigned char read_last[] = {0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
        const uint32_t last_address = 0x10000 + fresh_size - 4096;
        memcpy(read_last + 3, &last_address, sizeof(last_address));
        memcpy(code, target, sizeof(target));
        memcpy(code + 0x100, patch, sizeof(patch));
        memcpy(code + 0x200, read_first, sizeof(read_first));
        memcpy(code + 0x300, read_last, sizeof(read_last));
        const uint32_t new_value = 73;
        memcpy(storage, &new_value, sizeof(new_value));
        memcpy(storage + fresh_size - 4096, &new_value, sizeof(new_value));
        const unsigned char fresh_code[] = {0xb8, 81, 0, 0, 0, 0xc3};
        if (fresh_exec) {
            memcpy(storage + 0x100, fresh_code, sizeof(fresh_code));
            memcpy(storage + fresh_size - 4096 + 0x100, fresh_code, sizeof(fresh_code));
        }
        LumenQemuTcgMemoryRegion regions[] = {
            {0x1000, 4096, code, 7, 0},
            {0xe000, 4096, storage + 0xe000, 3, 0},
            {0x10000, fresh_size, storage, fresh_exec ? 7u : 3u, 0},
        };
        Fixture fixture = {.bytes = storage, .regions = regions, .region_count = 2};
        const LumenQemuTcgRunConfig config = config_for(&fixture);
        LumenQemuTcgSession *root = NULL, *peer = NULL;
        CHECK(LumenQemuTcgOpen(&config, &root) == LUMEN_QEMU_TCG_READY);
        LumenQemuTcgGuestState state = state_for(&fixture);
        CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 42);
        fixture.region_count = 3;
        CHECK(LumenQemuTcgInvalidateAdditive(0x10000, fresh_size) == LUMEN_QEMU_TCG_READY);
        /* Native ranges are truly disjoint: only RAMBlock's rounded padding
           can cover the adjacent older code. Touch both ends of the fresh
           logical view, then translate the code on a cold nested CPU. */
        state = state_for(&fixture); state.rip = 0x1200;
        CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == new_value);
        state = state_for(&fixture); state.rip = 0x1300;
        CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == new_value);
        if (fresh_exec) {
            state = state_for(&fixture); state.rip = 0x10100;
            CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 81);
            state = state_for(&fixture); state.rip = last_address + 0x100;
            CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 81);
        }
        CHECK(LumenQemuTcgOpen(&config, &peer) == LUMEN_QEMU_TCG_READY);
        state = state_for(&fixture);
        CHECK(run_to_boundary(peer, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 42);
        state = state_for(&fixture); state.rip = 0x1100;
        CHECK(run_to_boundary(peer, &state) == LUMEN_QEMU_TCG_RETURNED);
        state = state_for(&fixture);
        CHECK(run_to_boundary(peer, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 43);
        LumenQemuTcgClose(peer);
        state = state_for(&fixture);
        CHECK(run_to_boundary(root, &state) == LUMEN_QEMU_TCG_RETURNED && state.gprs[0] == 43);
        LumenQemuTcgClose(root);
        CHECK(LumenQemuTcgInvalidate(0, UINT64_MAX) == LUMEN_QEMU_TCG_READY);
        free(storage);
    }
    printf("Additive padding/SMC: six NX and six EXEC split-page layouts, cold nested CPU and warm root, host_page=%ld\n",
        sysconf(_SC_PAGESIZE));
    return 0;
}

#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
static int code_ram_equivalence(void)
{
    Fixture fixture = {0};
    const bool parallel = option_enabled("LUMEN_QEMU_TCG_PARALLEL");
    CHECK(posix_memalign((void **)&fixture.bytes, 16384, 131072) == 0);
    memset(fixture.bytes, 0, 131072);
    const uint64_t stack_pc = UINT64_C(0x70000000);
    const uint64_t return_pc = RETURN_PC;
    memcpy(fixture.bytes + 98304, &return_pc, sizeof(return_pc));
    const unsigned char code42[] = {0xb8, 42, 0, 0, 0, 0xc3};
    const unsigned char code43[] = {0xb8, 43, 0, 0, 0, 0xc3};
    for (uint32_t original = 0; original < 2; ++original) {
        for (uint32_t layout = 0; layout < 4; ++layout) {
            CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
            LumenQemuTcgCodeRamTestReset(original);
            LumenQemuTcgMemoryRegion views[3] = {
                {0x10000, 4096, fixture.bytes, 7, 0},
                {0x20000, 4096, fixture.bytes, 7, 0},
                {stack_pc, 16384, fixture.bytes + 98304, 3, 0}
            };
            uint64_t pc_a = views[0].guest_base;
            uint64_t pc_b = views[1].guest_base;
            if (layout == 1) { /* Partial native alias of two real 32K blocks. */
                views[0].size = views[1].size = 32768;
                views[1].host_base = fixture.bytes + 16384;
                pc_a += 16384;
            } else if (layout == 2) { /* Logical 4K disjoint; actual Mac RAMBlock16K. */
                views[1].host_base = fixture.bytes + 4096;
            } else if (layout == 3) { /* Same main index even at QEMU's max TLB size. */
                views[1].guest_base = views[0].guest_base + UINT64_C(0x100000000);
                views[1].host_base = fixture.bytes + 16384;
                pc_b = views[1].guest_base;
            }
            memcpy((unsigned char *)views[0].host_base + (pc_a - views[0].guest_base),
                   code42, sizeof(code42));
            if (layout >= 2) memcpy(views[1].host_base, code43, sizeof(code43));
            fixture.regions = views;
            fixture.region_count = 3;
            LumenQemuTcgRunConfig config = config_for(&fixture);
            LumenQemuTcgSession *session = NULL;
            CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
            LumenQemuTcgCodeRamProbe probe = {0};
            CHECK(LumenQemuTcgCodeRamTestProbe(session, pc_a, &views[1], &probe) ==
                  LUMEN_QEMU_TCG_READY);
            CHECK(probe.ram_offset == probe.canonical_offset);
            CHECK(probe.view_length == views[0].size);
            CHECK(probe.block_length >= probe.view_length);
            const bool overlapping = layout < 2 ||
                (layout == 2 && probe.block_length > 4096);
            CHECK(probe.warm_owner_resolved && probe.owner_resolved);
            CHECK(!probe.warm_rejected);
            if (layout == 2) {
                CHECK(probe.block_length == (uint64_t)sysconf(_SC_PAGESIZE));
                printf("Code-RAM padding: host_page=%ld view=%llu actual=%llu overlap=%u\n",
                       sysconf(_SC_PAGESIZE), (unsigned long long)probe.view_length,
                       (unsigned long long)probe.block_length, overlapping);
            }
            /* Pre-map every view before the guest/victim sequence. */
            CHECK(LumenQemuTcgCodeRamTestProbe(session, pc_b, NULL, &probe) ==
                  LUMEN_QEMU_TCG_READY);
            if (layout == 3) {
                CHECK(LumenQemuTcgCodeRamTestProbe(session, pc_a, NULL, &probe) ==
                      LUMEN_QEMU_TCG_READY);
                CHECK(LumenQemuTcgCodeRamTestProbe(session, pc_b, NULL, &probe) ==
                      LUMEN_QEMU_TCG_READY);
                const uint32_t queries = fixture.query_count;
                CHECK(LumenQemuTcgCodeRamTestProbe(session, pc_a, NULL, &probe) ==
                      LUMEN_QEMU_TCG_READY);
                CHECK(probe.victim_present && fixture.query_count == queries);
                CHECK(probe.ram_offset == probe.canonical_offset);
            }
            for (uint32_t pass = 0; pass < 3; ++pass) {
                LumenQemuTcgGuestState state = state_for(&fixture);
                state.rip = pass == 1 ? pc_b : pc_a;
                state.gprs[4] = stack_pc;
                CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
                CHECK(state.gprs[0] == ((pass == 1 && layout >= 2) ? 43 : 42));
            }
            LumenQemuTcgClose(session);
            LumenQemuTcgCodeRamStats stats;
            LumenQemuTcgCodeRamTestRead(&stats);
            CHECK(stats.mismatch == 0);
            CHECK(original ? stats.fast == 0 && stats.reference > 0 : stats.fast > 0);
            if (layout == 2 && overlapping) CHECK(stats.padded > 0);
            printf("Code-RAM resolved-owner reference: reference=%u parallel=%u layout=%u fast=%llu fallback=%llu reference_calls=%llu padded=%llu original_divergence=%llu mismatch=%llu\n",
                   original, parallel, layout, (unsigned long long)stats.fast,
                   (unsigned long long)stats.fallback,
                   (unsigned long long)stats.reference,
                   (unsigned long long)stats.padded,
                   (unsigned long long)stats.original_divergence,
                   (unsigned long long)stats.mismatch);
        }
    }
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgCodeRamTestReset(0);
    free(fixture.bytes);
    return 0;
}
#endif


/* Eight persistent CPU-only roots compete with a ninth root whose HLE boundary
 * waits for native readiness at 256/48000 seconds. No audio device, guest OS,
 * production telemetry or guest-time scaling is involved. This is a liveness
 * and state fixture; its recorded host timings do not qualify game FPS/audio. */
#define SCHEDULER_PURE_PEERS 8u
#define SCHEDULER_PEERS (SCHEDULER_PURE_PEERS + 1u)
#define SCHEDULER_PERIODS 64u

typedef struct SchedulerFixture {
    Fixture *fixture;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    _Atomic uint32_t ready;
    _Atomic uint32_t completed_blocks;
    _Atomic bool stop;
    bool start;
    uint32_t produced;
    uint64_t ready_ns[SCHEDULER_PERIODS];
} SchedulerFixture;

typedef struct SchedulerPeer {
    SchedulerFixture *shared;
    unsigned int index;
    _Atomic(LumenQemuTcgSession *) session;
    bool valid;
    uint64_t quanta;
    uint64_t progress;
    uint64_t max_run_call_ns;
    uint64_t total_ready_to_boundary_ns;
    uint64_t max_ready_to_boundary_ns;
} SchedulerPeer;

static uint64_t scheduler_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool scheduler_state_valid(const SchedulerPeer *peer,
                                  const LumenQemuTcgGuestState *state)
{
    if (state->gprs[12] != UINT64_C(0xc0ffee0000000000) + peer->index ||
        state->rflags != 0x203 || state->mxcsr != 0x1f80 || state->x87_control != 0x37f) {
        return false;
    }
    for (unsigned int reg = 0; reg < 16; ++reg) {
        for (unsigned int byte = 0; byte < 32; ++byte) {
            if (state->ymm[reg][byte] != (uint8_t)(peer->index + reg + byte)) return false;
        }
    }
    return true;
}

static void *scheduler_peer(void *opaque)
{
    SchedulerPeer *peer = opaque;
    SchedulerFixture *shared = peer->shared;
    const bool readiness_peer = peer->index == SCHEDULER_PURE_PEERS;
    LumenQemuTcgRunConfig config = config_for(shared->fixture);
    LumenQemuTcgSession *session = NULL;
    peer->valid = LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY;
    atomic_store(&peer->session, session);
    atomic_fetch_add(&shared->ready, 1);
    pthread_mutex_lock(&shared->mutex);
    while (!shared->start && !atomic_load(&shared->stop)) {
        pthread_cond_wait(&shared->condition, &shared->mutex);
    }
    pthread_mutex_unlock(&shared->mutex);
    if (!peer->valid) return NULL;

    LumenQemuTcgGuestState state = {0};
    state.struct_size = sizeof(state);
    state.abi_version = 1;
    state.rip = readiness_peer ? 0x6100 : 0x6000;
    state.gprs[4] = 0x8000 + peer->index * 256;
    state.gprs[12] = UINT64_C(0xc0ffee0000000000) + peer->index;
    state.rflags = 0x203;
    state.mxcsr = 0x1f80;
    state.x87_control = 0x37f;
    for (unsigned int reg = 0; reg < 16; ++reg) {
        for (unsigned int byte = 0; byte < 32; ++byte) {
            state.ymm[reg][byte] = (uint8_t)(peer->index + reg + byte);
        }
    }
    uint64_t previous_block = 0;
    uint64_t previous_ready_ns = 0;
    uint32_t consumed = 0;
    while (!atomic_load(&shared->stop)) {
        const uint64_t before = scheduler_now_ns();
        const uint32_t result = LumenQemuTcgRun(session, &state);
        const uint64_t after = scheduler_now_ns();
        const uint64_t elapsed = after - before;
        if (elapsed > peer->max_run_call_ns) peer->max_run_call_ns = elapsed;
        if (!scheduler_state_valid(peer, &state) || state.gprs[0] < peer->progress) {
            peer->valid = false;
            break;
        }
        peer->progress = state.gprs[0];
        if (result == LUMEN_QEMU_TCG_INTERRUPTED) {
            ++peer->quanta;
            continue;
        }
        if (!readiness_peer || result != LUMEN_QEMU_TCG_HOST_CALL ||
            state.boundary_target != shared->fixture->host_pc ||
            state.gprs[0] != previous_block + 256 ||
            state.gprs[4] != 0x8000 + peer->index * 256 - 8) {
            peer->valid = false;
            break;
        }
        previous_block = state.gprs[0];
        if (previous_ready_ns != 0) {
            const uint64_t latency = after - previous_ready_ns;
            peer->total_ready_to_boundary_ns += latency;
            if (latency > peer->max_ready_to_boundary_ns) peer->max_ready_to_boundary_ns = latency;
            if (atomic_fetch_add(&shared->completed_blocks, 1) + 1 == SCHEDULER_PERIODS) break;
        }
        /* Run has returned and released admission before native semaphore wait.
         * The fixture owns stable code/backing and distinct stacks until all peers join. */
        pthread_mutex_lock(&shared->mutex);
        while (consumed == shared->produced && !atomic_load(&shared->stop)) {
            pthread_cond_wait(&shared->condition, &shared->mutex);
        }
        if (atomic_load(&shared->stop)) {
            pthread_mutex_unlock(&shared->mutex);
            break;
        }
        previous_ready_ns = shared->ready_ns[consumed++];
        pthread_mutex_unlock(&shared->mutex);
        memcpy(&state.rip, shared->fixture->bytes + state.gprs[4], sizeof(state.rip));
        state.gprs[4] += 8;
    }
    atomic_store(&peer->session, NULL);
    LumenQemuTcgClose(session);
    return NULL;
}

static int scheduler_fairness(Fixture *fixture)
{
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char pure[] = {0x48, 0x8d, 0x40, 1, 0xeb, 0xfa}; /* lea rax,[rax+1]; jmp */
    unsigned char block[] = {
        0xb9, 0, 1, 0, 0,              /* mov ecx,256 */
        0x48, 0x8d, 0x40, 1,           /* lea rax,[rax+1] */
        0xe2, 0xfa,                    /* loop back to lea; preserves carry */
        0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, /* mov r11,host_pc */
        0x41, 0xff, 0xd3,              /* call r11 */
        0xeb, 0xe6                     /* jmp start */
    };
    memcpy(block + 13, &fixture->host_pc, sizeof(fixture->host_pc));
    memcpy(fixture->bytes + 0x6000, pure, sizeof(pure));
    memcpy(fixture->bytes + 0x6100, block, sizeof(block));
    const uint64_t return_pc = RETURN_PC;
    for (unsigned int i = 0; i < SCHEDULER_PEERS; ++i) {
        memcpy(fixture->bytes + 0x8000 + i * 256, &return_pc, sizeof(return_pc));
    }
    SchedulerFixture shared = {.fixture = fixture,
        .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    SchedulerPeer peers[SCHEDULER_PEERS] = {0};
    pthread_t threads[SCHEDULER_PEERS];
    unsigned int created = 0;
    bool valid = true;
    for (; created < SCHEDULER_PEERS; ++created) {
        peers[created].shared = &shared;
        peers[created].index = created;
        if (pthread_create(&threads[created], NULL, scheduler_peer, &peers[created]) != 0) {
            valid = false;
            break;
        }
    }
    const struct timespec poll = {0, 1000000};
    uint64_t deadline = scheduler_now_ns() + UINT64_C(3000000000);
    while (atomic_load(&shared.ready) != created && scheduler_now_ns() < deadline) nanosleep(&poll, NULL);
    valid = valid && atomic_load(&shared.ready) == SCHEDULER_PEERS;
    pthread_mutex_lock(&shared.mutex);
    shared.start = true;
    if (!valid) atomic_store(&shared.stop, true);
    pthread_cond_broadcast(&shared.condition);
    pthread_mutex_unlock(&shared.mutex);
    /* Relative sleeps deliberately tolerate host scheduling. Timestamp actual
     * readiness rather than claiming 48 kHz playback or a strict 2 ms bound. */
    const struct timespec period = {0, 256 * 1000000000LL / 48000};
    for (unsigned int i = 0; valid && i < SCHEDULER_PERIODS; ++i) {
        nanosleep(&period, NULL);
        pthread_mutex_lock(&shared.mutex);
        shared.ready_ns[i] = scheduler_now_ns();
        ++shared.produced;
        pthread_cond_broadcast(&shared.condition);
        pthread_mutex_unlock(&shared.mutex);
    }
    deadline = scheduler_now_ns() + UINT64_C(3000000000);
    while (valid && atomic_load(&shared.completed_blocks) != SCHEDULER_PERIODS &&
           scheduler_now_ns() < deadline) nanosleep(&poll, NULL);
    valid = valid && atomic_load(&shared.completed_blocks) == SCHEDULER_PERIODS;
    atomic_store(&shared.stop, true);
    pthread_mutex_lock(&shared.mutex);
    pthread_cond_broadcast(&shared.condition);
    pthread_mutex_unlock(&shared.mutex);
    for (unsigned int i = 0; i < created; ++i) {
        LumenQemuTcgSession *session = atomic_load(&peers[i].session);
        if (session != NULL) LumenQemuTcgInterrupt(session);
    }
    for (unsigned int i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) valid = false;
        valid = valid && peers[i].valid && peers[i].progress != 0;
        if (i < SCHEDULER_PURE_PEERS) valid = valid && peers[i].quanta >= 2;
        printf("Scheduler peer: index=%u role=%s quanta=%llu progress=%llu max_run_call_ns=%llu\n",
               i, i < SCHEDULER_PURE_PEERS ? "pure-cpu" : "native-readiness",
               (unsigned long long)peers[i].quanta, (unsigned long long)peers[i].progress,
               (unsigned long long)peers[i].max_run_call_ns);
    }
    const SchedulerPeer *readiness = &peers[SCHEDULER_PURE_PEERS];
    printf("Scheduler readiness: completed=%u frames_per_block=256 nominal_hz=48000 "
           "sum_ready_to_boundary_ns=%llu max_ready_to_boundary_ns=%llu metric=synthetic_liveness "
           "game_fps=unmeasured audible_quality=unmeasured\n",
           atomic_load(&shared.completed_blocks),
           (unsigned long long)readiness->total_ready_to_boundary_ns,
           (unsigned long long)readiness->max_ready_to_boundary_ns);
    pthread_cond_destroy(&shared.condition);
    pthread_mutex_destroy(&shared.mutex);
    CHECK(valid);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    return 0;
}

static int warm_boundary_targets(Fixture *fixture)
{
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char direct[] = {0xe8, 0xfb, 0, 0, 0, 0xc3};
    unsigned char indirect[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
                                0xff, 0xd0, 0xc3};
    const uint64_t target = 0x6100;
    memcpy(indirect + 2, &target, sizeof(target));
    const unsigned char callee[] = {0xb8, 42, 0, 0, 0, 0xc3};
    memcpy(fixture->bytes + 0x6000, direct, sizeof(direct));
    memcpy(fixture->bytes + 0x6020, indirect, sizeof(indirect));
    memcpy(fixture->bytes + target, callee, sizeof(callee));
    LumenQemuTcgRunConfig config = config_for(fixture);
    LumenQemuTcgSession *session = NULL;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    for (unsigned int warm = 0; warm < 2; ++warm) {
        for (unsigned int indirect_call = 0; indirect_call < 2; ++indirect_call) {
            LumenQemuTcgGuestState state = state_for(fixture);
            state.rip = indirect_call ? 0x6020 : 0x6000;
            CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
            CHECK(state.gprs[0] == 42 && state.rip == RETURN_PC);
        }
    }
    const uint64_t native_target = fixture->host_pc;
    fixture->host_pc = target;
    for (unsigned int indirect_call = 0; indirect_call < 2; ++indirect_call) {
        LumenQemuTcgGuestState state = state_for(fixture);
        state.rip = indirect_call ? 0x6020 : 0x6000;
        CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_HOST_CALL);
        CHECK(state.rip == target && state.boundary_target == target && state.gprs[4] == 0xdff8);
        /* The result proves that even a linked/cached target did not execute
         * its mov eax,42 before the dynamic HLE interception. */
        CHECK(state.gprs[0] == (indirect_call ? target : 0));
        memcpy(&state.rip, fixture->bytes + state.gprs[4], sizeof(state.rip));
        state.gprs[4] += 8;
        state.gprs[0] = 99;
        CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(state.gprs[0] == 99 && state.rip == RETURN_PC);
    }
    fixture->host_pc = native_target;
    LumenQemuTcgClose(session);
    /* Return targets are per-root configuration. The already warm callee
     * must return at entry for this root, then execute for the following root. */
    config.return_target = target;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgGuestState state = state_for(fixture);
    state.rip = 0x6000;
    state.gprs[0] = UINT64_C(0xc0ffee);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.rip == target && state.gprs[0] == UINT64_C(0xc0ffee) && state.gprs[4] == 0xdff8);
    LumenQemuTcgClose(session);
    config.return_target = RETURN_PC;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(fixture);
    state.rip = 0x6000;
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.rip == RETURN_PC && state.gprs[0] == 42);
    LumenQemuTcgClose(session);
    const unsigned char compare[] = {
        0x83, 0xf8, 1,                       /* cmp eax,1 */
        0xe9, 0xf8, 0, 0, 0                 /* jmp 0x6300 */
    };
    const unsigned char flags[] = {
        0x0f, 0x94, 0xc3,                   /* setz bl */
        0x0f, 0x92, 0xc1, 0xc3              /* setb cl; ret */
    };
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    memcpy(fixture->bytes + 0x6200, compare, sizeof(compare));
    memcpy(fixture->bytes + 0x6300, flags, sizeof(flags));
    for (unsigned int unqualified = 0; unqualified < 2; ++unqualified) {
        config.flags = unqualified ? 0 : LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT;
        fixture->disturb_host_jit = unqualified != 0;
        CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
        for (unsigned int warm = 0; warm < 4; ++warm) {
            for (unsigned int zero = 0; zero < 2; ++zero) {
                state = state_for(fixture);
                state.rip = 0x6200;
                state.gprs[0] = zero;
                state.mxcsr = 0x1f80 | (zero ? 0x2000 : 0x4000);
                state.x87_control = 0x37f | (zero ? 0x400 : 0x800);
                CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
                /* The compare and consumers occupy different TBs. A linked
                 * entry hook must preserve lazy x86 condition codes while
                 * classifying the target, including an uncertified callback
                 * that changes this thread's native JIT protection. */
                CHECK(state.gprs[0] == zero && state.gprs[3] == zero && state.gprs[1] == !zero);
                CHECK((state.rflags & 0x41) == (zero ? 0x40 : 1));
                CHECK(state.mxcsr == (0x1f80 | (zero ? 0x2000 : 0x4000)) &&
                      state.x87_control == (0x37f | (zero ? 0x400 : 0x800)));
            }
        }
        LumenQemuTcgClose(session);
    }
    fixture->disturb_host_jit = false;
    printf("Warm boundaries: direct_hle=passed indirect_hle=passed per_root_return_target=passed "
           "linked_carry_zero_fp=passed chaining=%u\n",
           option_enabled("LUMEN_QEMU_TCG_CHAINING"));
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    return 0;
}

typedef struct ParallelOverlap {
    Fixture *fixture;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool start;
    _Atomic uint32_t ready;
    _Atomic uint32_t probed;
    _Atomic uint32_t inside;
    _Atomic uint32_t overlap_mask;
    _Atomic bool overlap_started;
    _Atomic bool prior_guest_stores;
} ParallelOverlap;

typedef struct ParallelPeer {
    ParallelOverlap *shared;
    unsigned int index;
    bool valid;
    uint32_t result;
} ParallelPeer;

static int parallel_memory_query(void *opaque, uint64_t address, uint64_t size,
                                 uint32_t access, LumenQemuTcgMemoryRegion *region)
{
    ParallelPeer *peer = opaque;
    ParallelOverlap *shared = peer->shared;
    const uint32_t bit = 1u << peer->index;
    if (access == LUMEN_QEMU_TCG_READ && address == 0x7000 + peer->index * 4096 &&
        !(atomic_fetch_or(&shared->probed, bit) & bit)) {
        /* This cold RAM access follows a real guest LOCK store on each CPU.
         * Hold the guest helper, not an admission counter. A serial Run cannot
         * reach the peer callback until this callback has returned. */
        atomic_fetch_or(&shared->inside, bit);
        const uint64_t deadline = scheduler_now_ns() + UINT64_C(100000000);
        const struct timespec delay = {0, 1000000};
        while (!atomic_load(&shared->overlap_started) && scheduler_now_ns() < deadline) {
            if (atomic_load(&shared->inside) == 3) {
                atomic_store(&shared->overlap_started, true);
                break;
            }
            nanosleep(&delay, NULL);
        }
        if (atomic_load(&shared->overlap_started)) {
            uint64_t first, second;
            memcpy(&first, shared->fixture->bytes + 0x9000, sizeof(first));
            memcpy(&second, shared->fixture->bytes + 0x9008, sizeof(second));
            if (first == 1 && second == 1) atomic_store(&shared->prior_guest_stores, true);
            atomic_fetch_or(&shared->overlap_mask, bit);
        }
        atomic_fetch_and(&shared->inside, ~bit);
    }
    return memory_query(shared->fixture, address, size, access, region);
}

static int parallel_host_query(void *opaque, uint64_t pc)
{
    ParallelPeer *peer = opaque;
    return host_query(peer->shared->fixture, pc);
}

static LumenQemuTcgGuestState parallel_state(const ParallelPeer *peer, uint64_t pc)
{
    LumenQemuTcgGuestState state = {0};
    state.struct_size = sizeof(state);
    state.abi_version = 1;
    state.rip = pc;
    state.gprs[4] = 0xa000 + peer->index * 0x2000;
    state.gprs[8] = 0x9000 + peer->index * 8;
    state.gprs[9] = 0x7000 + peer->index * 4096;
    state.gprs[12] = UINT64_C(0xc0ffee0000000000) + peer->index;
    state.fs_base = 0x1200 + peer->index * 0x100;
    state.gs_base = 0x2400 + peer->index * 0x100;
    state.rflags = 0x203;
    state.mxcsr = 0x1f80 | (peer->index ? 0x2000 : 0);
    state.x87_control = 0x37f | (peer->index ? 0x400 : 0);
    for (unsigned int reg = 0; reg < 16; ++reg)
        for (unsigned int byte = 0; byte < 32; ++byte)
            state.ymm[reg][byte] = (uint8_t)(peer->index + reg + byte);
    return state;
}

static void *parallel_overlap_worker(void *opaque)
{
    ParallelPeer *peer = opaque;
    ParallelOverlap *shared = peer->shared;
    LumenQemuTcgRunConfig config = config_for(shared->fixture);
    config.memory_query = parallel_memory_query;
    config.host_target_query = parallel_host_query;
    config.opaque = peer;
    LumenQemuTcgSession *session = NULL;
    peer->valid = LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY;
    if (peer->valid) {
        LumenQemuTcgGuestState warm = parallel_state(peer, 0x6100);
        peer->valid = run_to_boundary(session, &warm) == LUMEN_QEMU_TCG_RETURNED;
    }
    atomic_fetch_add(&shared->ready, 1);
    pthread_mutex_lock(&shared->mutex);
    while (!shared->start) pthread_cond_wait(&shared->condition, &shared->mutex);
    pthread_mutex_unlock(&shared->mutex);
    if (!peer->valid) {
        if (session != NULL) LumenQemuTcgClose(session);
        return NULL;
    }
    LumenQemuTcgGuestState state = parallel_state(peer, 0x6000);
    peer->result = run_to_boundary(session, &state);
    peer->valid = peer->result == LUMEN_QEMU_TCG_RETURNED && state.rip == RETURN_PC &&
        state.gprs[3] == UINT64_C(0x1111111111111111) + peer->index &&
        state.gprs[12] == UINT64_C(0xc0ffee0000000000) + peer->index &&
        state.fs_base == 0x1200 + peer->index * 0x100 &&
        state.gs_base == 0x2400 + peer->index * 0x100 && state.rflags == 0x203 &&
        state.mxcsr == (0x1f80 | (peer->index ? 0x2000 : 0)) &&
        state.x87_control == (0x37f | (peer->index ? 0x400 : 0));
    for (unsigned int reg = 0; reg < 16; ++reg)
        for (unsigned int byte = 0; byte < 32; ++byte)
            peer->valid = peer->valid && state.ymm[reg][byte] == (uint8_t)(peer->index + reg + byte);
    LumenQemuTcgClose(session);
    return NULL;
}

static int parallel_guest_overlap(Fixture *fixture)
{
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char guest[] = {
        0xf0, 0x49, 0xff, 0x00,                   /* lock inc qword [r8] */
        0x49, 0x8b, 0x19,                         /* mov rbx,[r9]: cold helper rendezvous */
        0xf0, 0x48, 0xff, 0x04, 0x25, 0, 0x20, 0, 0, /* lock inc [0x2000] */
        0xf0, 0x48, 0xff, 0x04, 0x25, 0xfd, 0x2f, 0, 0, /* cross-page lock inc [0x2ffd] */
        0x68, 3, 2, 0, 0, 0x9d, 0xc3             /* push 0x203; popfq; ret */
    };
    const unsigned char warm[] = {
        0x48, 0x8b, 0x04, 0x25, 0, 0x90, 0, 0, /* mov rax,[0x9000] */
        0x48, 0x8b, 0x04, 0x25, 0, 0x20, 0, 0, /* mov rax,[0x2000] */
        0x48, 0x8b, 0x04, 0x25, 0, 0x30, 0, 0, 0xc3 /* mov rax,[0x3000]; ret */
    };
    memcpy(fixture->bytes + 0x6000, guest, sizeof(guest));
    memcpy(fixture->bytes + 0x6100, warm, sizeof(warm));
    const uint64_t zero = 0, return_pc = RETURN_PC;
    memcpy(fixture->bytes + 0x2000, &zero, sizeof(zero));
    memcpy(fixture->bytes + 0x2ffd, &zero, sizeof(zero));
    memcpy(fixture->bytes + 0x9000, &zero, sizeof(zero));
    memcpy(fixture->bytes + 0x9008, &zero, sizeof(zero));
    for (unsigned int i = 0; i < 2; ++i) {
        const uint64_t value = UINT64_C(0x1111111111111111) + i;
        memcpy(fixture->bytes + 0x7000 + i * 4096, &value, sizeof(value));
        memcpy(fixture->bytes + 0xa000 + i * 0x2000, &return_pc, sizeof(return_pc));
    }
    ParallelOverlap shared = {.fixture = fixture,
        .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    ParallelPeer peers[2] = {{.shared = &shared, .index = 0}, {.shared = &shared, .index = 1}};
    pthread_t threads[2];
    unsigned int created = 0;
    bool valid = true;
    for (; created < 2; ++created) {
        if (pthread_create(&threads[created], NULL, parallel_overlap_worker, &peers[created]) != 0) {
            valid = false;
            break;
        }
    }
    const struct timespec delay = {0, 1000000};
    const uint64_t deadline = scheduler_now_ns() + UINT64_C(3000000000);
    while (atomic_load(&shared.ready) != created && scheduler_now_ns() < deadline) nanosleep(&delay, NULL);
    valid = valid && atomic_load(&shared.ready) == 2;
    pthread_mutex_lock(&shared.mutex);
    shared.start = true;
    pthread_cond_broadcast(&shared.condition);
    pthread_mutex_unlock(&shared.mutex);
    for (unsigned int i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) valid = false;
        valid = valid && peers[i].valid;
    }
    uint64_t aligned, cross_page, flags[2];
    memcpy(&aligned, fixture->bytes + 0x2000, sizeof(aligned));
    memcpy(&cross_page, fixture->bytes + 0x2ffd, sizeof(cross_page));
    memcpy(flags, fixture->bytes + 0x9000, sizeof(flags));
    const bool parallel = option_enabled("LUMEN_QEMU_TCG_PARALLEL");
    const uint32_t overlap = atomic_load(&shared.overlap_mask);
    printf("Guest overlap: parallel=%u helper_overlap_mask=%u prior_guest_lock_stores=%u "
           "aligned_atomic=%llu cross_page_atomic=%llu state=%s metric=semantic_concurrency\n",
           parallel, overlap, atomic_load(&shared.prior_guest_stores),
           (unsigned long long)aligned, (unsigned long long)cross_page, valid ? "passed" : "failed");
    pthread_cond_destroy(&shared.condition);
    pthread_mutex_destroy(&shared.mutex);
    CHECK(valid && aligned == 2 && cross_page == 2 && flags[0] == 1 && flags[1] == 1);
    CHECK(atomic_load(&shared.probed) == 3 && overlap == (parallel ? 3 : 0));
    CHECK(atomic_load(&shared.prior_guest_stores) == parallel);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    return 0;
}

static int scheduler_parser_before_initialize(void)
{
    const char *value = getenv("LUMEN_QEMU_TCG_SCHEDULER");
    char *saved = value != NULL ? strdup(value) : NULL;
    CHECK(value == NULL || saved != NULL);
    const char *invalid[] = {"", "FIFO", "fair", "fair-budget ", " fifo", "250"};
    bool valid = true;
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        valid = valid && setenv("LUMEN_QEMU_TCG_SCHEDULER", invalid[i], 1) == 0;
        valid = valid && LumenQemuTcgInitialize(32) == LUMEN_QEMU_TCG_INVALID_ARGUMENT;
        valid = valid && strstr(LumenQemuTcgReceipt(), "uninitialized") != NULL;
    }
    if (saved != NULL) {
        valid = setenv("LUMEN_QEMU_TCG_SCHEDULER", saved, 1) == 0 && valid;
    } else {
        valid = unsetenv("LUMEN_QEMU_TCG_SCHEDULER") == 0 && valid;
    }
    free(saved);
    CHECK(valid);
    return 0;
}

static int scheduler_latch_after_initialize(void)
{
    const char *value = getenv("LUMEN_QEMU_TCG_SCHEDULER");
    char *saved = value != NULL ? strdup(value) : NULL;
    CHECK(value == NULL || saved != NULL);
    const bool changed = setenv("LUMEN_QEMU_TCG_SCHEDULER", "invalid-after-initialize", 1) == 0;
    const uint32_t result = changed ? LumenQemuTcgInitialize(32) : LUMEN_QEMU_TCG_INTERNAL_ERROR;
    const bool restored = saved != NULL
        ? setenv("LUMEN_QEMU_TCG_SCHEDULER", saved, 1) == 0
        : unsetenv("LUMEN_QEMU_TCG_SCHEDULER") == 0;
    free(saved);
    CHECK(changed && restored && result == LUMEN_QEMU_TCG_READY);
    return 0;
}

static int boolean_options_before_initialize(void)
{
    const char *names[] = {"LUMEN_QEMU_TCG_SCHEDULER_TRACE", "LUMEN_QEMU_TCG_JIT_EXEC_CACHE",
                           "LUMEN_QEMU_TCG_CHAINING", "LUMEN_QEMU_TCG_PARALLEL",
                           "LUMEN_QEMU_TCG_DEFER_RECLAIM", "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK",
                           "LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING"};
    for (unsigned int option = 0; option < sizeof(names) / sizeof(names[0]); ++option) {
        const char *value = getenv(names[option]);
        char *saved = value != NULL ? strdup(value) : NULL;
        CHECK(value == NULL || saved != NULL);
#ifndef __APPLE__
        if (strcmp(names[option], "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK") == 0) {
            CHECK(setenv(names[option], "1", 1) == 0);
            CHECK(LumenQemuTcgInitialize(32) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
            CHECK(strstr(LumenQemuTcgReceipt(), "uninitialized") != NULL);
        }
#endif
        const char *invalid[] = {"", "true", "2", " 1", "1 "};
        bool valid = true;
        for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            valid = valid && setenv(names[option], invalid[i], 1) == 0;
            valid = valid && LumenQemuTcgInitialize(32) == LUMEN_QEMU_TCG_INVALID_ARGUMENT;
            valid = valid && strstr(LumenQemuTcgReceipt(), "uninitialized") != NULL;
        }
        valid = (saved != NULL ? setenv(names[option], saved, 1) : unsetenv(names[option])) == 0 && valid;
        free(saved);
        CHECK(valid);
    }
    return 0;
}

static int boolean_options_latched(void)
{
    const char *names[] = {"LUMEN_QEMU_TCG_SCHEDULER_TRACE", "LUMEN_QEMU_TCG_JIT_EXEC_CACHE",
                           "LUMEN_QEMU_TCG_CHAINING", "LUMEN_QEMU_TCG_PARALLEL",
                           "LUMEN_QEMU_TCG_DEFER_RECLAIM", "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK",
                           "LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING"};
    for (unsigned int option = 0; option < sizeof(names) / sizeof(names[0]); ++option) {
        const char *value = getenv(names[option]);
        char *saved = value != NULL ? strdup(value) : NULL;
        CHECK(value == NULL || saved != NULL);
        const bool changed = setenv(names[option], "invalid-after-initialize", 1) == 0;
        const uint32_t result = changed ? LumenQemuTcgInitialize(32) : LUMEN_QEMU_TCG_INTERNAL_ERROR;
        const bool restored = (saved != NULL ? setenv(names[option], saved, 1) : unsetenv(names[option])) == 0;
        free(saved);
        CHECK(changed && restored && result == LUMEN_QEMU_TCG_READY);
    }
    return 0;
}

static int callback_contracts(Fixture *fixture)
{
    LumenQemuTcgRunConfig config = config_for(fixture);
    LumenQemuTcgSession *session = NULL;
    config.flags = 2;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    config.flags = LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT;
    config.reserved = 1;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    config.reserved = 0;
    config.abi_version = 1;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    config.abi_version = 2;
    config.struct_size = offsetof(LumenQemuTcgRunConfig, flags);
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
    const char *jit = getenv("LUMEN_QEMU_TCG_JIT_EXEC_CACHE");
    for (unsigned int version = 0; version < 3; ++version) {
        config = config_for(fixture);
        if (version == 0) {
            config.abi_version = 1;
            config.struct_size = offsetof(LumenQemuTcgRunConfig, flags);
            /* These bytes are outside the v1 ABI and must never be imported. */
            config.flags = UINT32_MAX;
            config.reserved = UINT32_MAX;
        } else if (version == 1) {
            config.flags = 0;
        }
        fixture->disturb_host_jit = version < 2;
        /* Legacy does not promise to recover an arbitrary callback changing
         * permissions mid-TB. Exercise that new restoration only when enabled. */
        fixture->disturb_memory_jit = version == 2 && jit && strcmp(jit, "1") == 0;
        CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
        CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
        for (unsigned int warm = 0; warm < 2; ++warm) {
            LumenQemuTcgGuestState state = state_for(fixture);
            CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
            CHECK(state.gprs[0] == 42 && state.rip == RETURN_PC);
        }
        LumenQemuTcgClose(session);
    }
    if (jit && strcmp(jit, "1") == 0) {
        /* The immediate spans two code pages. The second fetch occurs in
         * translation's WRITE scope, while cold stack access returns through
         * generated code in EXECUTE mode. The callback changes real protection
         * in both contexts, so successful completion proves both restorations. */
        const uint64_t expected = UINT64_C(0x123456789abcdef0);
        unsigned char crossing[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xc3};
        memcpy(crossing + 2, &expected, sizeof(expected));
        unsigned char previous[sizeof(crossing)];
        memcpy(previous, fixture->bytes + 0x2ffc, sizeof(previous));
        CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
        memcpy(fixture->bytes + 0x2ffc, crossing, sizeof(crossing));
        config = config_for(fixture);
        fixture->disturb_host_jit = false;
        fixture->disturb_memory_jit = true;
        fixture->memory_jit_execute = true;
        CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
        LumenQemuTcgGuestState state = state_for(fixture);
        state.rip = 0x2ffc;
        CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(state.gprs[0] == expected && state.rip == RETURN_PC);
        LumenQemuTcgClose(session);
        CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
        memcpy(fixture->bytes + 0x2ffc, previous, sizeof(previous));
    }
    fixture->disturb_host_jit = false;
    fixture->disturb_memory_jit = false;
    fixture->memory_jit_execute = false;
#ifdef __APPLE__
    CHECK(fixture->jit_host_changes > 0);
    if (jit && strcmp(jit, "1") == 0) CHECK(fixture->jit_memory_changes > 0);
#endif
    printf("Callback contracts: legacy_v1=passed certified_v2=passed unqualified_v2=passed "
           "unknown_flags=denied reserved=denied host_protection_changes=%u memory_protection_changes=%u\n",
           fixture->jit_host_changes, fixture->jit_memory_changes);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    return 0;
}

static void retirement_code(Fixture *fixture, uint32_t value)
{
    unsigned char code[] = {
        0xb8, 0, 0, 0, 0, /* mov eax, value */
        0x48, 0x89, 0x04, 0x25, 0, 0x20, 0, 0, /* mov [0x2000], rax */
        0xc3
    };
    memcpy(code + 1, &value, sizeof(value));
    memset(fixture->bytes, 0, 65536);
    memcpy(fixture->bytes + 4096, code, sizeof(code));
}

static int retirement_warm(LumenQemuTcgSession *session, Fixture *fixture,
                           uint32_t expected, uint64_t *checksum)
{
    for (unsigned int warm = 0; warm < 2; ++warm) {
        LumenQemuTcgGuestState state = state_for(fixture);
        CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
        uint64_t value = 0;
        memcpy(&value, fixture->bytes + 0x2000, sizeof(value));
        CHECK(state.gprs[0] == expected && value == expected && state.rip == RETURN_PC);
        *checksum += value;
    }
    return 0;
}

#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
typedef struct ReclaimReader {
    _Atomic uint32_t ready, release, result;
} ReclaimReader;

static void *reclaim_reader(void *opaque)
{
    ReclaimReader *reader = opaque;
    uint32_t result = LumenQemuTcgReclaimTestRcuHold(1);
    atomic_store(&reader->result, result);
    atomic_store(&reader->ready, 1);
    const struct timespec delay = {0, 1000000};
    while (result == LUMEN_QEMU_TCG_READY && !atomic_load(&reader->release))
        nanosleep(&delay, NULL);
    if (result == LUMEN_QEMU_TCG_READY)
        atomic_store(&reader->result, LumenQemuTcgReclaimTestRcuHold(0));
    return NULL;
}
#endif

static int retirement_lifetime(unsigned int rounds, bool benchmark)
{
    Fixture fixture = {.whole_region = true};
    fixture.host_pc = (uint64_t)(uintptr_t)&native_host_function;
    fixture.bytes = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(fixture.bytes != MAP_FAILED);
    CHECK(LumenQemuTcgInvalidate(0, UINT64_MAX) == LUMEN_QEMU_TCG_READY);
    retirement_code(&fixture, 101);
    LumenQemuTcgRunConfig config = config_for(&fixture);
    LumenQemuTcgSession *session = NULL;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    LumenQemuTcgReclaimTestDrain();
    LumenQemuTcgReclaimStats before = {0}, after = {0};
    LumenQemuTcgReclaimTestRead(&before);
    const char *option = getenv("LUMEN_QEMU_TCG_DEFER_RECLAIM");
    const bool held_reader = !benchmark && option && strcmp(option, "1") == 0;
    ReclaimReader reader = {0};
    pthread_t reader_thread;
    if (held_reader) {
        CHECK(pthread_create(&reader_thread, NULL, reclaim_reader, &reader) == 0);
        const struct timespec delay = {0, 1000000};
        for (unsigned int i = 0; i < 1000 && !atomic_load(&reader.ready); ++i)
            nanosleep(&delay, NULL);
        CHECK(atomic_load(&reader.ready) && atomic_load(&reader.result) == LUMEN_QEMU_TCG_READY);
    }
#endif
    uint64_t checksum = 0;
    const uint64_t start = scheduler_now_ns();
    for (unsigned int round = 0; round < rounds; ++round) {
        CHECK(retirement_warm(session, &fixture, round + 101, &checksum) == 0);
        if (!benchmark && round % 31 == 0) {
            /* Both root CPUs hold warm mappings of the same backing. */
            LumenQemuTcgSession *nested = NULL;
            CHECK(LumenQemuTcgOpen(&config, &nested) == LUMEN_QEMU_TCG_READY);
            uint64_t nested_checksum = 0;
            CHECK(retirement_warm(nested, &fixture, round + 101, &nested_checksum) == 0);
            CHECK(nested_checksum == 2 * (uint64_t)(round + 101));
            LumenQemuTcgClose(nested);
        }
        CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
        /* The previous guest bytes become inaccessible immediately, while
         * verification deliberately keeps their FlatView/RAMBlock alive.
         * Alternate real unmap and MAP_FIXED replacement at the same host
         * address: stale TLBs, TBs or host-byte reclamation would fail here. */
        CHECK(mprotect(fixture.bytes, 65536, PROT_NONE) == 0);
        unsigned char *address = fixture.bytes;
        if ((round & 1) == 0) CHECK(munmap(address, 65536) == 0);
        fixture.bytes = mmap(address, 65536, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        CHECK(fixture.bytes == address);
        retirement_code(&fixture, round + 102);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
        if (held_reader && round == 7) {
            LumenQemuTcgReclaimTestRead(&after);
            CHECK(after.pending_batches == 8 && after.fence_pairs == before.fence_pairs);
            CHECK(after.pending_maps == 8 && after.pending_bytes >= 8 * UINT64_C(65536));
            atomic_store(&reader.release, 1);
            CHECK(pthread_join(reader_thread, NULL) == 0);
            CHECK(atomic_load(&reader.result) == LUMEN_QEMU_TCG_READY);
        }
#endif
    }
    const uint64_t elapsed = scheduler_now_ns() - start;
    CHECK(checksum == (uint64_t)rounds * (rounds + 201));
    LumenQemuTcgClose(session);
    CHECK(mprotect(fixture.bytes, 65536, PROT_NONE) == 0);
    CHECK(munmap(fixture.bytes, 65536) == 0);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    LumenQemuTcgReclaimTestRead(&after);
    CHECK(after.pending_batches < after.batch_limit && after.pending_maps < 512 &&
          after.pending_bytes < UINT64_C(256) * 1024 * 1024);
    if (held_reader) {
        CHECK(after.deferred_batches - before.deferred_batches == rounds);
        CHECK(after.fence_pairs - before.fence_pairs >= rounds / after.batch_limit);
        CHECK(after.max_pending_batches <= after.batch_limit);
    }
    /* Old callbacks now complete against an entirely unmapped host range. */
    LumenQemuTcgReclaimTestDrain();
    LumenQemuTcgReclaimTestRead(&after);
    CHECK(after.pending_batches == 0 && after.pending_maps == 0 && after.pending_bytes == 0);
    printf("Retirement metadata: deferred=%llu fence_pairs=%llu guard_fallbacks=%llu max_pending=%u\n",
           (unsigned long long)(after.deferred_batches - before.deferred_batches),
           (unsigned long long)(after.fence_pairs - before.fence_pairs),
           (unsigned long long)(after.guard_fallbacks - before.guard_fallbacks),
           after.max_pending_batches);
#endif
    printf("%s rounds=%u checksum=%llu elapsed_ns=%llu protection=none reuse=same_host_va\n",
           benchmark ? "RETIREMENT_BENCHMARK" : "Retirement lifetime",
           rounds, (unsigned long long)checksum, (unsigned long long)elapsed);
    return 0;
}

#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
static int retirement_guard_fallback(void)
{
    /* This callback actually reads retired host bytes. The capability guard
     * must make it complete before Invalidate returns, while they are RW. */
    LumenQemuTcgReclaimTestDrain();
    Fixture fixture = {.whole_region = true};
    fixture.host_pc = (uint64_t)(uintptr_t)&native_host_function;
    fixture.bytes = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(fixture.bytes != MAP_FAILED);
    retirement_code(&fixture, 73);
    fixture.bytes[0] = 0x9a;
    LumenQemuTcgReclaimStats before = {0}, after = {0};
    LumenQemuTcgReclaimTestRead(&before);
    CHECK(LumenQemuTcgReclaimTestNotifier(1, fixture.bytes, 65536) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgRunConfig config = config_for(&fixture);
    LumenQemuTcgSession *session = NULL;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    uint64_t checksum = 0;
    CHECK(retirement_warm(session, &fixture, 73, &checksum) == 0 && checksum == 146);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgReclaimTestRead(&after);
    CHECK(after.notifier_removals - before.notifier_removals == 1);
    CHECK(after.notifier_failures == before.notifier_failures && after.pending_batches == 0);
    const char *option = getenv("LUMEN_QEMU_TCG_DEFER_RECLAIM");
    if (option && strcmp(option, "1") == 0) {
        CHECK(after.guard_fallbacks - before.guard_fallbacks == 1);
        CHECK(after.fence_pairs - before.fence_pairs == 1);
    }
    CHECK(LumenQemuTcgReclaimTestNotifier(0, NULL, 0) == LUMEN_QEMU_TCG_READY);
    CHECK(mprotect(fixture.bytes, 65536, PROT_NONE) == 0);
    CHECK(munmap(fixture.bytes, 65536) == 0);
    LumenQemuTcgReclaimTestDrain();
    printf("Retirement guard fallback: host_read_callback=completed_before_return sentinel=passed\n");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1) {
        CHECK(argc <= 3 && strcmp(argv[1], "--retirement-benchmark") == 0);
        unsigned long rounds = 128;
        if (argc == 3) {
            char *end = NULL;
            CHECK(argv[2][0] >= '0' && argv[2][0] <= '9');
            rounds = strtoul(argv[2], &end, 10);
            CHECK(end && *end == '\0');
        }
        CHECK(rounds >= 16 && rounds <= 1024 && rounds % 16 == 0);
        CHECK(LumenQemuTcgInitialize(32) == LUMEN_QEMU_TCG_READY);
        return retirement_lifetime((unsigned int)rounds, true);
    }
    Fixture fixture = {0};
    fixture.host_pc = (uint64_t)(uintptr_t)&native_host_function;
    CHECK(posix_memalign((void **)&fixture.bytes, 4096, 65536) == 0);
    memset(fixture.bytes, 0, 65536);
    CHECK(scheduler_parser_before_initialize() == 0);
    CHECK(boolean_options_before_initialize() == 0);
    CHECK(LumenQemuTcgInitialize(32) == LUMEN_QEMU_TCG_READY);
    CHECK(scheduler_latch_after_initialize() == 0);
    CHECK(boolean_options_latched() == 0);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgRunConfig config = config_for(&fixture);
    LumenQemuTcgSession *session = NULL;
    const unsigned char arithmetic[] = {
        0x48, 0xc7, 0xc0, 0x27, 0, 0, 0,
        0x48, 0x83, 0xc0, 3,
        0x48, 0x89, 0x04, 0x25, 0, 0x20, 0, 0,
        0xc3
    };
    memcpy(fixture.bytes + 4096, arithmetic, sizeof(arithmetic));
    /* Initialize and the first Open intentionally occur on different
     * native threads, as with a loader dispatching module constructors. */
    pthread_t first_worker;
    CHECK(pthread_create(&first_worker, NULL, first_open_worker, &fixture) == 0);
    CHECK(pthread_join(first_worker, NULL) == 0);
    CHECK(atomic_load(&fixture.worker_result) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgGuestState state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    uint64_t value;
    memcpy(&value, fixture.bytes + 0x2000, sizeof(value));
    CHECK(value == 42 && state.gprs[0] == 42 && state.rip == RETURN_PC);
    LumenQemuTcgClose(session);
    CHECK(callback_contracts(&fixture) == 0);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    fixture.denied_page = 0x2000;
    fixture.denied_access = LUMEN_QEMU_TCG_WRITE;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_GUEST_FAULT);
    CHECK(state.fault_address == 0x2000 && state.fault_exception == 14 &&
          state.fault_access == LUMEN_QEMU_TCG_WRITE);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char read[] = {0x48, 0x8b, 0x04, 0x25, 0, 0x20, 0, 0, 0xc3};
    memcpy(fixture.bytes + 4096, read, sizeof(read));
    fixture.denied_page = 0x2000;
    fixture.denied_access = LUMEN_QEMU_TCG_READ;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_GUEST_FAULT);
    CHECK(state.fault_address == 0x2000 && state.fault_access == LUMEN_QEMU_TCG_READ);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    memcpy(fixture.bytes + 4096, arithmetic, sizeof(arithmetic));
    fixture.denied_page = 0x1000;
    fixture.denied_access = LUMEN_QEMU_TCG_EXECUTE;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_GUEST_FAULT);
    CHECK(state.fault_address == 0x1000 && state.fault_access == LUMEN_QEMU_TCG_EXECUTE);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    fixture.denied_access = 0;

    unsigned char atomic[] = {0xf0, 0x48, 0xff, 0x04, 0x25, 0, 0x20, 0, 0, 0xc3};
    memcpy(fixture.bytes + 4096, atomic, sizeof(atomic));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    memcpy(&value, fixture.bytes + 0x2000, sizeof(value));
    CHECK(value == 43);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const uint32_t unaligned_address = 0x2ffd;
    memcpy(atomic + 5, &unaligned_address, sizeof(unaligned_address));
    memcpy(fixture.bytes + 4096, atomic, sizeof(atomic));
    value = 41;
    memcpy(fixture.bytes + unaligned_address, &value, sizeof(value));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    memcpy(&value, fixture.bytes + unaligned_address, sizeof(value));
    CHECK(value == 42);
    LumenQemuTcgClose(session);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);

    unsigned char hle[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
    uint64_t host_pc = fixture.host_pc;
    memcpy(hle + 2, &host_pc, sizeof(host_pc));
    memcpy(fixture.bytes + 4096, hle, sizeof(hle));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_HOST_CALL);
    CHECK(state.boundary_target == fixture.host_pc && state.gprs[4] == 0xdff8);
    LumenQemuTcgSession *nested;
    CHECK(LumenQemuTcgOpen(&config, &nested) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgGuestState nested_state = state_for(&fixture);
    nested_state.rip = RETURN_PC;
    CHECK(run_to_boundary(nested, &nested_state) == LUMEN_QEMU_TCG_RETURNED);
    LumenQemuTcgClose(nested);
    memcpy(&state.rip, fixture.bytes + state.gprs[4], sizeof(state.rip));
    state.gprs[4] += 8;
    state.gprs[0] = native_host_function(42);
    LumenQemuTcgInterrupt(session);
    CHECK(LumenQemuTcgRun(session, &state) == LUMEN_QEMU_TCG_INTERRUPTED);
    CHECK(state.gprs[0] == 99 && state.rip == 0x100c);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.gprs[0] == 99);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char direct_call[] = {0xe8, 0xfb, 0, 0, 0, 0xc3};
    const unsigned char cached_target[] = {0x48, 0xc7, 0xc0, 42, 0, 0, 0, 0xc3};
    memcpy(fixture.bytes + 4096, direct_call, sizeof(direct_call));
    memcpy(fixture.bytes + 0x1100, cached_target, sizeof(cached_target));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    for (unsigned int i = 0; i < 2; ++i) {
        state = state_for(&fixture);
        CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
        CHECK(state.gprs[0] == 42);
    }
    /* A target newly identified as a host boundary must remain observable
     * even when the direct call and its guest TB were already cached. */
    fixture.host_pc = 0x1100;
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_HOST_CALL);
    CHECK(state.boundary_target == 0x1100 && state.gprs[4] == 0xdff8);
    fixture.host_pc = (uint64_t)(uintptr_t)&native_host_function;
    memcpy(&state.rip, fixture.bytes + state.gprs[4], sizeof(state.rip));
    state.gprs[4] += 8;
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char syscall[] = {0x0f, 0x05, 0xc3};
    memcpy(fixture.bytes + 4096, syscall, sizeof(syscall));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_SYSCALL);
    CHECK(state.gprs[1] == 0x1002);
    state.rip = state.gprs[1];
    state.rflags = state.gprs[11];
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char tsc[] = {0x0f, 0x31, 0xc3};
    memcpy(fixture.bytes + 4096, tsc, sizeof(tsc));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    uint64_t tsc_before = LumenQemuTcgReadTsc();
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    uint64_t guest_tsc = state.gprs[0] | (state.gprs[2] << 32);
    uint64_t tsc_after = LumenQemuTcgReadTsc();
    CHECK(guest_tsc >= tsc_before && guest_tsc <= tsc_after);
    CHECK(LumenQemuTcgTscFrequency() == 1000000000);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char cpuid_clock[] = {0xb8, 0x15, 0, 0, 0, 0x31, 0xc9, 0x0f, 0xa2, 0xc3};
    memcpy(fixture.bytes + 4096, cpuid_clock, sizeof(cpuid_clock));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.gprs[0] == 1 && state.gprs[3] == 1 &&
          state.gprs[1] == LumenQemuTcgTscFrequency() && state.gprs[2] == 0);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char cpuid_features[] = {0xb8, 7, 0, 0, 0, 0x31, 0xc9, 0x0f, 0xa2, 0xc3};
    memcpy(fixture.bytes + 4096, cpuid_features, sizeof(cpuid_features));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK((state.gprs[3] & 1) != 0); /* FSGSBASE */
    CHECK((state.gprs[3] & (1u << 5)) != 0); /* AVX2 */
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char cpuid_xsave[] = {0xb8, 1, 0, 0, 0, 0x0f, 0xa2, 0xc3};
    memcpy(fixture.bytes + 4096, cpuid_xsave, sizeof(cpuid_xsave));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK((state.gprs[1] & (7u << 26)) == (7u << 26)); /* XSAVE, OSXSAVE, AVX */
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char xgetbv[] = {0x31, 0xc9, 0x0f, 0x01, 0xd0, 0xc3};
    memcpy(fixture.bytes + 4096, xgetbv, sizeof(xgetbv));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.gprs[0] == 7 && state.gprs[2] == 0);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    const unsigned char fsgsbase[] = {
        0xf3, 0x48, 0x0f, 0xae, 0xc0, /* rdfsbase rax */
        0xf3, 0x48, 0x0f, 0xae, 0xc9, /* rdgsbase rcx */
        0xf3, 0x48, 0x0f, 0xae, 0xd1, /* wrfsbase rcx */
        0xf3, 0x48, 0x0f, 0xae, 0xd8, /* wrgsbase rax */
        0xc3
    };
    memcpy(fixture.bytes + 4096, fsgsbase, sizeof(fsgsbase));
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    state.fs_base = 0x2340;
    state.gs_base = 0x3450;
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.gprs[0] == 0x2340 && state.gprs[1] == 0x3450);
    CHECK(state.fs_base == 0x3450 && state.gs_base == 0x2340);
    LumenQemuTcgClose(session);

    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    fixture.bytes[4096] = 0xeb;
    fixture.bytes[4097] = 0xfe;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    state = state_for(&fixture);
    state.gprs[0] = UINT64_C(0x123456789abcdef0);
    for (unsigned int i = 0; i < 3; ++i) {
        CHECK(LumenQemuTcgRun(session, &state) == LUMEN_QEMU_TCG_INTERRUPTED);
        CHECK(state.rip == 4096 && state.gprs[0] == UINT64_C(0x123456789abcdef0));
        CHECK(state.rflags == 0x202 && state.mxcsr == 0x1f80 && state.x87_control == 0x37f);
    }
    LumenQemuTcgClose(session);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, interrupt_worker, &fixture) == 0);
    const struct timespec delay = {0, 1000000};
    for (unsigned int i = 0; i < 1000 && !atomic_load(&fixture.worker_session); ++i) {
        nanosleep(&delay, NULL);
    }
    CHECK(atomic_load(&fixture.worker_session) != NULL);
    LumenQemuTcgInterrupt(atomic_load(&fixture.worker_session));
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(atomic_load(&fixture.worker_result) == LUMEN_QEMU_TCG_INTERRUPTED);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    CHECK(LumenQemuTcgInvalidate(0, 65536) == LUMEN_QEMU_TCG_READY);
    CHECK(multi_cpu_invalidation(&fixture) == 0);
    CHECK(additive_map_retention(&fixture) == 0);
    CHECK(additive_padding_smc() == 0);
    CHECK(scheduler_fairness(&fixture) == 0);
    CHECK(warm_boundary_targets(&fixture) == 0);
    CHECK(parallel_guest_overlap(&fixture) == 0);
    CHECK(retirement_lifetime(320, false) == 0);
#ifdef LUMEN_QEMU_TCG_CODE_RAM_VERIFY
    /* Exercise the actual CPU work queue, including an intervening work
       item and a new commit queued after its predecessor was dequeued.
       The persistent-CPU SMC/backing/revoke cases above and below separately
       check that topology changes still affect guest execution. */
    CHECK(LumenQemuTcgCodeRamTestQueueCoalescing() == LUMEN_QEMU_TCG_READY);
    CHECK(retirement_guard_fallback() == 0);
    CHECK(code_ram_equivalence() == 0);
    fixture.whole_region = true;
    for (uint32_t original = 0; original < 2; ++original) {
        LumenQemuTcgCodeRamTestReset(original);
        CHECK(multi_cpu_invalidation(&fixture) == 0);
        LumenQemuTcgCodeRamStats stats;
        LumenQemuTcgCodeRamTestRead(&stats);
        CHECK(stats.mismatch == 0);
        CHECK(original ? (stats.fast == 0 && stats.reference > 0) : stats.fast > 0);
        printf("Code-RAM multi-CPU SMC/backing/revoke: reference=%u fast=%llu fallback=%llu mismatch=%llu\n",
               original, (unsigned long long)stats.fast,
               (unsigned long long)stats.fallback, (unsigned long long)stats.mismatch);
    }
#endif
    printf("PASS: first Open on a worker, arithmetic, owned RAM, read/write/fetch denial, aligned/cross-page atomics, HLE boundaries, nested roots, syscall boundary, TSC/CPUID clock domain, FSGSBASE and XCR0 feature contracts, repeated pure-CPU quanta, threaded interrupted boundary, batch map invalidation, persistent multi-CPU SMC/backing replacement/execute revocation, scheduler policy parser/process latch, nine-root pure-CPU/native-readiness state, warm direct/indirect HLE and per-root return targets, and guest helper overlap with serial negative control\n");
    free(fixture.bytes);
    return 0;
}
