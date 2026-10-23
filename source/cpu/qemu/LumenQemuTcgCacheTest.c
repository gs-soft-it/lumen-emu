/* SPDX-License-Identifier: GPL-2.0-only */
#include "LumenQemuTcg.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CODE_PC UINT64_C(0x1000)
#define CODE_SIZE UINT64_C(4096)
#define DATA_PC UINT64_C(0x8000)
#define DATA_SIZE UINT64_C(32768)
#define STACK_PC UINT64_C(0xff00)
#define HOST_PC UINT64_C(0x00007ffffffe0000)
#define RETURN_PC UINT64_C(0x00007fffffff0000)
#define QUOTA_FILL_SEEDS 104u
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

typedef struct Fixture {
    unsigned char *code;
    unsigned char *data;
    size_t code_mapping_size;
    uint64_t code_pc;
    bool writable_code;
    bool unproven_code;
    bool simd;
    bool disturb_query_jit;
    uint64_t query_jit_changes;
    uint64_t host_boundary_queries;
} Fixture;

static int memory_query(void *opaque, uint64_t address, uint64_t size,
                        uint32_t access, LumenQemuTcgMemoryRegion *region)
{
    Fixture *fixture = opaque;
#ifdef __APPLE__
    if (fixture->disturb_query_jit) {
        /* Memory queries carry no JIT-preservation promise. In particular
         * cache identity queries can occur during native code generation. */
        pthread_jit_write_protect_np(1);
        ++fixture->query_jit_changes;
    }
#endif
    /* The code mapping is anonymous and private, has no physical aliases
     * and remains host read-only while a session is open. Mutation occurs
     * only after Close plus Invalidate. The unproven negative deliberately
     * withholds this promise despite retaining the same guest RX rights. */
    const LumenQemuTcgMemoryRegion regions[] = {
        {fixture->code_pc, CODE_SIZE, fixture->code,
         LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_EXECUTE |
             (fixture->writable_code ? LUMEN_QEMU_TCG_WRITE : 0),
         !fixture->writable_code && !fixture->unproven_code ?
             LUMEN_QEMU_TCG_IMMUTABLE_CODE : 0},
        {DATA_PC, DATA_SIZE, fixture->data,
         LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_WRITE, 0}
    };
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        const LumenQemuTcgMemoryRegion *candidate = &regions[i];
        if (address >= candidate->guest_base &&
            address - candidate->guest_base < candidate->size &&
            size <= candidate->size - (address - candidate->guest_base) &&
            (candidate->permissions & access) == access) {
            *region = *candidate;
            return 1;
        }
    }
    return 0;
}

static int host_query(void *opaque, uint64_t pc)
{
    Fixture *fixture = opaque;
    if (pc != HOST_PC) return 0;
    ++fixture->host_boundary_queries;
    return 1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static int install_code(Fixture *fixture, unsigned int variant)
{
    /* Only the immediate changes between variants. Identical guest PC and
     * CPU state therefore cannot authorize reuse of stale translated IR. */
    if (mprotect(fixture->code, fixture->code_mapping_size,
                 PROT_READ | PROT_WRITE) != 0) return 0;
    memset(fixture->code, 0xcc, CODE_SIZE);
    const unsigned char entry[] = {
        0x48, 0x83, 0x47, 0x50, 1,           /* add qword [rdi+80], 1 */
        0xb8, 7, 0, 0, 0,                   /* mov eax, initial */
        0xb9, 9, 0, 0, 0,                   /* mov ecx, 9 */
        0xeb, 0x0f                           /* jmp code+32 */
    };
    const unsigned char loop[] = {
        0x6b, 0xc0, 3,                       /* imul eax, eax, 3 */
        0x01, 0xc8,                          /* add eax, ecx */
        0x48, 0x89, 0x04, 0xcf,              /* mov [rdi+rcx*8], rax */
        0xff, 0xc9,                          /* dec ecx */
        0x75, 0xf3,                          /* jnz code+32 */
        0xeb, 0x31                           /* jmp code+96 */
    };
    const unsigned char host_call[] = {
        0x48, 0x89, 0xc6,                    /* mov rsi, rax */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, /* mov rax, HOST_PC */
        0xff, 0xd0,                          /* call rax */
        0x48, 0x89, 0x07,                    /* mov [rdi], rax */
        0x48, 0x33, 0x47, 0x08,              /* xor rax, [rdi+8] */
        0xc3                                 /* ret */
    };
    const unsigned char simd[] = {0x0f, 0x57, 0xc0}; /* xorps xmm0, xmm0 */
    const size_t prefix = fixture->simd ? sizeof(simd) : 0;
    if (prefix) memcpy(fixture->code, simd, sizeof(simd));
    memcpy(fixture->code + prefix, entry, sizeof(entry));
    fixture->code[prefix + sizeof(entry) - 1] -= prefix;
    const uint32_t initial = 7 + variant;
    memcpy(fixture->code + prefix + 6, &initial, sizeof(initial));
    memcpy(fixture->code + 32, loop, sizeof(loop));
    memcpy(fixture->code + 96, host_call, sizeof(host_call));
    const uint64_t host_pc = HOST_PC;
    memcpy(fixture->code + 101, &host_pc, sizeof(host_pc));
    /* These bytes are read as guest x86 instructions, never executed by the
     * host. Guest permissions independently expose an immutable RX page. */
    if (!fixture->writable_code &&
        mprotect(fixture->code, fixture->code_mapping_size, PROT_READ) != 0)
        return 0;
    return 1;
}

static uint32_t run_to_boundary(LumenQemuTcgSession *session,
                                LumenQemuTcgGuestState *state)
{
    /* Keep quantum exits distinct from return/HLE. Bound a broken fixture
     * independently of the driver's process timeout. */
    for (unsigned int i = 0; i < 10000; ++i) {
        const uint32_t result = LumenQemuTcgRun(session, state);
        if (result != LUMEN_QEMU_TCG_INTERRUPTED) return result;
    }
    return LUMEN_QEMU_TCG_INTERNAL_ERROR;
}

static uint64_t host_function(uint64_t input)
{
    return input + 57;
}

static int execute_fixture(Fixture *fixture, unsigned int variant,
                           uint64_t *checksum, uint64_t *return_value)
{
    memset(fixture->data, 0, DATA_SIZE);
    const uint64_t return_pc = RETURN_PC;
    memcpy(fixture->data + STACK_PC - DATA_PC, &return_pc, sizeof(return_pc));
    LumenQemuTcgRunConfig config = {
        sizeof(config), 2, RETURN_PC, memory_query, host_query, fixture,
        LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT, 0
    };
    LumenQemuTcgSession *session = NULL;
    CHECK(LumenQemuTcgOpen(&config, &session) == LUMEN_QEMU_TCG_READY);
    LumenQemuTcgGuestState state = {0};
    state.struct_size = sizeof(state);
    state.abi_version = 1;
    state.rip = fixture->code_pc;
    state.gprs[4] = STACK_PC;
    state.gprs[7] = DATA_PC;
    state.rflags = 0x202;
    state.mxcsr = 0x1f80;
    state.x87_control = 0x37f;
    if (fixture->simd) memset(state.ymm[0], 0xa5, sizeof(state.ymm[0]));

    uint64_t expected[11] = {0};
    /* Pretranslation must never execute a guest side effect. A speculative
     * execution of the entry block would increment this counter twice. */
    expected[10] = 1;
    uint32_t arithmetic = 7 + variant;
    for (unsigned int counter = 9; counter != 0; --counter) {
        arithmetic = arithmetic * 3 + counter;
        expected[counter] = arithmetic;
    }
    expected[0] = host_function(arithmetic);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_HOST_CALL);
    CHECK(state.rip == HOST_PC && state.boundary_target == HOST_PC);
    CHECK(state.gprs[4] == STACK_PC - 8 && state.gprs[6] == arithmetic);
    uint64_t hle_return;
    memcpy(&hle_return, fixture->data + state.gprs[4] - DATA_PC,
           sizeof(hle_return));
    CHECK(hle_return == fixture->code_pc + 111);
    state.rip = hle_return;
    state.gprs[4] += 8;
    state.gprs[0] = host_function(state.gprs[6]);
    CHECK(run_to_boundary(session, &state) == LUMEN_QEMU_TCG_RETURNED);
    CHECK(state.rip == RETURN_PC && state.gprs[4] == STACK_PC + 8);
    CHECK(state.gprs[0] == (expected[0] ^ expected[1]));
    CHECK(state.gprs[1] == 0 && state.gprs[7] == DATA_PC);
    if (fixture->simd) {
        for (unsigned int i = 0; i < 16; ++i) CHECK(state.ymm[0][i] == 0);
        for (unsigned int i = 16; i < 32; ++i) CHECK(state.ymm[0][i] == 0xa5);
    }

    uint64_t actual[11];
    memcpy(actual, fixture->data, sizeof(actual));
    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    /* Include every guest store and the final register result so a stale
     * immediate cannot hide behind a coincidentally equal return value. */
    *checksum = state.gprs[0];
    for (unsigned int i = 0; i < 11; ++i)
        *checksum = (*checksum * UINT64_C(131)) ^ actual[i];
    *return_value = state.gprs[0];
    LumenQemuTcgClose(session);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned int variant = 0;
    bool replace = false, writable_code = false, run = false, reject = false;
    bool disturb_query_jit = false, unproven_code = false;
    bool fill_cache = false, simd = false, relocated = false, local_warm = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run") == 0) run = true;
        else if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc &&
                 (strcmp(argv[i + 1], "0") == 0 ||
                  strcmp(argv[i + 1], "1") == 0))
            variant = (unsigned int)(argv[++i][0] - '0');
        else if (strcmp(argv[i], "--replace-code") == 0) replace = true;
        else if (strcmp(argv[i], "--writable-code") == 0) writable_code = true;
        else if (strcmp(argv[i], "--expect-init-rejected") == 0) reject = true;
        else if (strcmp(argv[i], "--disturb-query-jit") == 0) disturb_query_jit = true;
        else if (strcmp(argv[i], "--unproven-code") == 0) unproven_code = true;
        else if (strcmp(argv[i], "--fill-cache") == 0) fill_cache = true;
        else if (strcmp(argv[i], "--overflow-unsupported") == 0) simd = true;
        else if (strcmp(argv[i], "--relocate-code") == 0) relocated = true;
        else if (strcmp(argv[i], "--quota-local-warm") == 0) local_warm = true;
        else {
            fprintf(stderr, "Usage: %s --run [--variant 0|1] "
                    "[--replace-code] [--writable-code] "
                    "[--expect-init-rejected] [--disturb-query-jit] "
                    "[--unproven-code] [--fill-cache] "
                    "[--overflow-unsupported] [--relocate-code] "
                    "[--quota-local-warm]\n", argv[0]);
            return 2;
        }
    }
    CHECK(run);
    CHECK(!fill_cache || (!replace && !simd && variant == 0));
    CHECK(!local_warm || fill_cache);
    if (reject) {
        CHECK(LumenQemuTcgInitialize(2) == LUMEN_QEMU_TCG_INVALID_ARGUMENT);
        printf("{\"result\":\"REJECTED\",\"initialization\":%u}\n",
               LUMEN_QEMU_TCG_INVALID_ARGUMENT);
        return 0;
    }
    const long host_page = sysconf(_SC_PAGESIZE);
    CHECK(host_page >= (long)CODE_SIZE && DATA_SIZE % host_page == 0);
    Fixture fixture = {.code_mapping_size = (size_t)host_page,
                       .code_pc = relocated ? UINT64_C(0x3000) : CODE_PC,
                       .writable_code = writable_code,
                       .disturb_query_jit = disturb_query_jit,
                       .unproven_code = unproven_code, .simd = simd};
    fixture.code = mmap(NULL, fixture.code_mapping_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fixture.data = mmap(NULL, DATA_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(fixture.code != MAP_FAILED && fixture.data != MAP_FAILED);
    CHECK(install_code(&fixture, variant));
    CHECK(LumenQemuTcgInitialize(2) == LUMEN_QEMU_TCG_READY);
    const uint64_t begin_ns = monotonic_ns();
    uint64_t checksum = 0, return_value = 0, first_checksum = 0;
    CHECK(execute_fixture(&fixture, variant, &checksum, &return_value) == 0);
    if (fill_cache) {
        first_checksum = checksum;
        for (variant = 1; variant < QUOTA_FILL_SEEDS; ++variant) {
            /* Real guest effects span many unique page identities in one
             * process. The writer quota must not limit JIT correctness. */
            CHECK(LumenQemuTcgInvalidate(fixture.code_pc, CODE_SIZE) == LUMEN_QEMU_TCG_READY);
            CHECK(install_code(&fixture, variant));
            CHECK(execute_fixture(&fixture, variant, &checksum, &return_value) == 0);
        }
        --variant;
        CHECK(checksum != first_checksum);
    }
    if (local_warm) {
        /* The static snapshot started empty. Publications in this process
         * must remain visible after the 512th store disables the writer. */
        CHECK(LumenQemuTcgInvalidate(fixture.code_pc, CODE_SIZE) == LUMEN_QEMU_TCG_READY);
        variant = 0;
        CHECK(install_code(&fixture, variant));
        CHECK(execute_fixture(&fixture, variant, &checksum, &return_value) == 0);
        CHECK(checksum == first_checksum);
    }
    if (replace) {
        first_checksum = checksum;
        /* The owner closes the session before revoking translations and
         * mutating the borrowed code. The second execution must see the
         * replacement bytes even with an existing in-process TB/cache. */
        CHECK(LumenQemuTcgInvalidate(fixture.code_pc, CODE_SIZE) == LUMEN_QEMU_TCG_READY);
        variant ^= 1;
        CHECK(install_code(&fixture, variant));
        CHECK(execute_fixture(&fixture, variant, &checksum, &return_value) == 0);
        CHECK(checksum != first_checksum);
    }
    const uint64_t elapsed_ns = monotonic_ns() - begin_ns;
    const unsigned int executions = fill_cache ? QUOTA_FILL_SEEDS + local_warm : replace ? 2u : 1u;
    CHECK(fixture.host_boundary_queries == executions);
    LumenQemuTcgCacheStats stats = {.struct_size = sizeof(stats), .abi_version = 1};
    CHECK(LumenQemuTcgReadCacheStats(&stats) == LUMEN_QEMU_TCG_READY);
    CHECK(LumenQemuTcgInvalidate(0, UINT64_C(65536)) == LUMEN_QEMU_TCG_READY);
    CHECK(munmap(fixture.code, fixture.code_mapping_size) == 0);
    CHECK(munmap(fixture.data, DATA_SIZE) == 0);
    printf("{\"result\":\"PASS\",\"variant\":%u,\"writable_code\":%s,"
           "\"immutable_proof\":%s,"
           "\"quota_fill\":%s,\"simd_exercised\":%s,"
           "\"code_pc\":%" PRIu64 ",\"quota_local_warm\":%s,"
           "\"replaced\":%s,\"hle_calls\":%u,\"checksum\":%" PRIu64 ","
           "\"host_boundary_queries\":%" PRIu64 ","
           "\"query_jit_changes\":%" PRIu64 ","
           "\"first_checksum\":%" PRIu64 ",\"return_value\":%" PRIu64 ","
           "\"elapsed_ns\":%" PRIu64 ",\"stats\":{"
           "\"hits\":%" PRIu64 ",\"misses\":%" PRIu64 ","
           "\"stores\":%" PRIu64 ",\"rejected\":%" PRIu64 ","
           "\"pretranslated\":%" PRIu64 "}}\n",
           variant, writable_code ? "true" : "false",
           !writable_code && !unproven_code ? "true" : "false",
           fill_cache ? "true" : "false", simd ? "true" : "false",
           fixture.code_pc, local_warm ? "true" : "false",
           replace ? "true" : "false", executions, checksum, fixture.host_boundary_queries,
           fixture.query_jit_changes,
           first_checksum, return_value, elapsed_ns,
           stats.hits, stats.misses, stats.stores, stats.rejected, stats.pretranslated);
    return 0;
}
