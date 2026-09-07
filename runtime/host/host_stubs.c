/*
 * Symbols the HLE library expects from the PPU loader sources.
 *
 * runtime/ppu/ and runtime/host/ are both excluded from the library target
 * (CMakeLists.txt), so a host that runs no lifted game still has to satisfy the
 * handful of out-of-line references the HLE modules make into the PPU loader.
 * These are inert but correct: a game-less host has no reservation state to
 * track, and the guest stores still have to be big-endian.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ppu/ppu_context.h"   /* ppu_context, PPU_THREAD_LOCAL */

extern uint8_t* vm_base;

/* ppu_loader.cpp - PPU reservation bookkeeping. The ppu_memory.h inlines call
 * these on every store so lwarx/stwcx reservations can be broken; with no PPU
 * threads there is never a live reservation. */
int  g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }

/* ppu_loader.cpp - out-of-line big-endian guest stores. */
void vm_write8(uint64_t addr, uint8_t v)
{
    vm_base[(uint32_t)addr] = v;
}

void vm_write32(uint64_t addr, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + (uint32_t)addr, &v, 4);
}

/* ppu_fs.cpp - VFS root consulted by cellGame/cellFs path mapping. */
const char* ppu_vfs_root = ".";

/* Inline write-watch hooks. ppu_memory.h expands every vm_write* into a range
 * test against g_ww_lo/g_ww_hi plus a call to ps3_ww_report_inline, and those
 * live in runtime/ppu/ppu_loader.cpp -- which CMake excludes from the runtime
 * library because it is compiled per-game. A game link therefore supplies them
 * and this host does not, so it failed to link with three undefined symbols.
 *
 * lo == hi leaves the watch permanently empty, so the test is a compare that
 * never fires and the reporter is never reached. */
unsigned int g_ww_lo = 0, g_ww_hi = 0;

void ps3_ww_report_inline(unsigned int addr, unsigned long long val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* ---------------------------------------------------------------------------
 * Diagnostics that live in the PPU boot scaffold.
 *
 * runtime/ppu/ is compiled per-game against the lifter's generated header, not
 * into this library, so an HLE module or syscall that calls one of its
 * diagnostic helpers has nothing to link against in a library-only build. These
 * are the do-nothing versions: the host harness has no guest context, no guest
 * stack and no lifted function table, so there is nothing for them to report.
 * -----------------------------------------------------------------------*/

PPU_THREAD_LOCAL ppu_context* g_active_ctx = 0;

uint32_t ppu_vm_size          = 0;
uint32_t g_barrier_sync_watch = 0;
uint32_t g_spu_image_src_ea   = 0;
uint32_t g_spu_image_ls_start = 0;
uint32_t g_spu_image_span     = 0;

uint32_t vm_read32(uint64_t a)
{
    uint32_t v;
    if (!vm_base) return 0;
    memcpy(&v, vm_base + (uint32_t)a, 4);
    return __builtin_bswap32(v);
}

void vm_write64(uint64_t a, uint64_t v)
{
    if (!vm_base) return;
    v = __builtin_bswap64(v);
    memcpy(vm_base + (uint32_t)a, &v, 8);
}

void ppu_resv_register(ppu_context* c)        { (void)c; }
void ppu_guard_page(uint32_t ea)              { (void)ea; }
void ppu_dump_guest_stack(ppu_context* c, const char* tag) { (void)c; (void)tag; }
void ppu_dump_bctrl_ring(uint32_t a, const char* tag)      { (void)a; (void)tag; }
void ppu_guest_callstack(const char* tag)     { (void)tag; }
void ppu_log_host_chain(const char* tag)      { (void)tag; }
void lbp_breadcrumb_dump(const char* tag)     { (void)tag; }

/* Names the guest function that called in. No lifted function table here, so
 * say so rather than leaving the caller's buffer undefined. */
void ppu_guest_caller(char* out, size_t n)
{
    if (out && n) snprintf(out, n, "<no guest context>");
}

uint32_t ps3_spu_image_source_ea(uint32_t img_ea) { return img_ea; }

/* The context-aware HLE table lives in ppu_hle.cpp, which is also per-game. */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}
