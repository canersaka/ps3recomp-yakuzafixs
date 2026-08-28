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
#include <string.h>

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
