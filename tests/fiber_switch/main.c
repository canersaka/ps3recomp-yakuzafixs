/*
 * ps3recomp - tests/fiber_switch/main.c
 *
 * cellFiber cooperative switching, on whatever mechanism the host uses:
 * Windows fibers on Windows, ucontext elsewhere. Apple marks the ucontext
 * routines deprecated and hides them behind _XOPEN_SOURCE, which raises the
 * fair question of whether they still work on arm64 -- so ask, several
 * thousand switches at a time, instead of assuming either way.
 *
 * The shape here is scheduler -> fiber -> yield -> scheduler, run thousands
 * of times, with each fiber checking the argument it was created with. A
 * fiber that starts with the wrong argument is the readable symptom of a
 * context save that landed outside the struct it was given.
 *
 * Exit code 0 = pass.
 */
#include "cellFiber.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Guest memory. cellFiber.c writes fiber ids and attribute fields through
 * vm_write32/vm_read32, so a plain host buffer standing in for guest memory
 * is all it needs (the same shim tests/sync_stress uses).
 * -----------------------------------------------------------------------*/
#define GUEST_MEM_SIZE (1u * 1024u * 1024u)
static uint8_t g_guest_mem[GUEST_MEM_SIZE];
uint8_t* vm_base = g_guest_mem;

/* ppu_memory.h's inline stores reference these. Nothing here watches guest
 * writes or holds a reservation. */
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

static uint32_t g_bump = 4096;   /* keep guest address 0 invalid */
static uint32_t guest_alloc(uint32_t size)
{
    uint32_t a = g_bump;
    g_bump += (size + 15u) & ~15u;
    return a;
}

static uint32_t read_be32(uint32_t addr)
{
    const uint8_t* p = g_guest_mem + addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---------------------------------------------------------------------------
 * Failure bookkeeping
 * -----------------------------------------------------------------------*/
static int g_fails = 0;
#define FAILF(...) do { g_fails++; fprintf(stderr, "[FAIL] "); \
                        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

#define SWITCHES  4000

/* ---------------------------------------------------------------------------
 * Case 1: the scheduler runs each fiber in turn and each yields straight back.
 * -----------------------------------------------------------------------*/
static CellFiber f_a, f_b;
static long ran_a, ran_b;
static long arg_mismatches;

static void entry_a(u64 arg)
{
    if (arg != 0xA5A5A5A5A5A5A5A5ull) arg_mismatches++;
    for (;;) { ran_a++; cellFiberPpuYieldFiber(); }
}

static void entry_b(u64 arg)
{
    if (arg != 0x5A5A5A5A5A5A5A5Aull) arg_mismatches++;
    for (;;) { ran_b++; cellFiberPpuYieldFiber(); }
}

static int case_scheduler_roundtrip(void)
{
    long before = g_fails;
    for (int i = 0; i < SWITCHES; i++) {
        s32 rc = cellFiberPpuSwitchFiber(f_a);
        if (rc != CELL_OK) { FAILF("switch to A on iteration %d: 0x%08X", i, (unsigned)rc); break; }
        rc = cellFiberPpuSwitchFiber(f_b);
        if (rc != CELL_OK) { FAILF("switch to B on iteration %d: 0x%08X", i, (unsigned)rc); break; }
    }
    if (ran_a != SWITCHES) FAILF("fiber A ran %ld times, want %d", ran_a, SWITCHES);
    if (ran_b != SWITCHES) FAILF("fiber B ran %ld times, want %d", ran_b, SWITCHES);
    if (arg_mismatches)    FAILF("%ld fiber entries saw the wrong arg", arg_mismatches);

    int ok = (g_fails == before);
    printf("[scheduler_roundtrip] A=%ld B=%ld -> %s\n", ran_a, ran_b, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------*/
int main(void)
{
    /* A switch that lands on the wrong stack takes the process down, so keep
     * the progress lines out of a buffer that dies with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ps3recomp fiber_switch ===\n");

    s32 rc = cellFiberPpuInitialize();
    if (rc != CELL_OK) { FAILF("Initialize: 0x%08X", (unsigned)rc); return 1; }

    /* Attributes come from guest memory: {const char* name; u32 priority;
     * u32 stackSize}, three 4-byte fields. */
    uint32_t attr = guest_alloc(16);
    rc = cellFiberPpuAttributeInitialize((CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) FAILF("AttributeInitialize: 0x%08X", (unsigned)rc);
    if (read_be32(attr + 8) != CELL_FIBER_DEFAULT_STACK_SIZE)
        FAILF("AttributeInitialize left stackSize = %u, want %u",
              read_be32(attr + 8), (unsigned)CELL_FIBER_DEFAULT_STACK_SIZE);

    uint32_t ha = guest_alloc(4), hb = guest_alloc(4);
    rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)ha, entry_a,
                                 0xA5A5A5A5A5A5A5A5ull,
                                 (const CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) { FAILF("CreateFiber A: 0x%08X", (unsigned)rc); return 1; }
    rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)hb, entry_b,
                                 0x5A5A5A5A5A5A5A5Aull,
                                 (const CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) { FAILF("CreateFiber B: 0x%08X", (unsigned)rc); return 1; }
    f_a = read_be32(ha);
    f_b = read_be32(hb);
    if (f_a == f_b) FAILF("both fibers got id %u", (unsigned)f_a);

    int rc1 = case_scheduler_roundtrip();

    rc = cellFiberPpuFinalize();
    if (rc != CELL_OK) FAILF("Finalize: 0x%08X", (unsigned)rc);

    int failed = rc1 || g_fails;
    printf("=== RESULT: %s (fails=%d) ===\n", failed ? "FAIL" : "PASS", g_fails);
    return failed ? 1 : 0;
}
