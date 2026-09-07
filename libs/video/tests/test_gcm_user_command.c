/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_user_command.c -Wl,-dead_strip
 *   -o /tmp/test_gcm_user_command && /tmp/test_gcm_user_command
 */
#include "../cellGcmSys.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
static unsigned calls;
static uint32_t last;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    if (opd == 0x200) { cellGcmQueueUserCommand(0xDD); return; }
    assert(opd == 0x100 && !b && !c && !d && !e && !f && !h && !i);
    calls++; last = (uint32_t)a;
    if (a == 0xAA) { cellGcmQueueUserCommand(0xBB); ppu_gcm_pump(); }
    return;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)0x100);
    cellGcmQueueUserCommand(1);
    cellGcmQueueUserCommand(0xAA);
    assert(calls == 0);
    ppu_gcm_pump();
    assert(calls == 1 && last == 0xAA);
    ppu_gcm_pump();
    assert(calls == 2 && last == 0xBB);
    ppu_gcm_pump(); assert(calls == 2);
    cellGcmQueueUserCommand(0); ppu_gcm_pump();
    assert(calls == 3 && last == 0);
    /* A vblank callback can publish another cause after this pump has
     * claimed the old interrupt. It must not replace that claimed cause. */
    s_vblank_handler_opd = 0x200;
    cellGcmQueueUserCommand(0xCC);
    cellGcmTickVBlank();
    ppu_gcm_pump(); assert(calls == 4 && last == 0xCC);
    s_vblank_handler_opd = 0;
    ppu_gcm_pump(); assert(calls == 5 && last == 0xDD);
    ppu_gcm_pump(); assert(calls == 5);
    cellGcmSetUserHandler(NULL);
    cellGcmQueueUserCommand(7); ppu_gcm_pump(); assert(calls == 5);
    puts("Deferred GCM user-command checks passed");
}
