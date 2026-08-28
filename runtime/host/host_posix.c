/*
 * ps3recomp - minimal POSIX host
 *
 * Drives the cellGcm -> RSX -> backend bridge with no lifted game: it builds a
 * real big-endian NV4097 command buffer by hand, hands it to the FIFO, and runs
 * the flip loop. That exercises the whole graphics path a real title uses to
 * clear and flip, which is exactly the state the D3D12 backend reaches today.
 *
 * Its value is as a bring-up and regression harness for backends. A backend
 * that renders the right colour here has its clear, surface and present paths
 * correct, and can be developed with no game binary and no recompiler run.
 *
 * Exit status: 0 on success, non-zero if the guest command stream did not reach
 * the backend intact -- so it works as a CI check.
 */
#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_metal_backend.h"

#include <ps3emu/guest_call.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guest address space this host hands to the HLE layer. */
uint8_t* vm_base = NULL;

#define VM_SIZE     (64u * 1024u * 1024u)
#define IO_ADDR     0x00100000u          /* RSX IO window base                */
#define IO_SIZE     0x00200000u
#define CMD_SIZE    0x00010000u
#define CTRL_ADDR   0x03002000u          /* DMA control: put @+0, get @+4     */
#define HEAP_BASE   0x00400000u

#define CLEAR_ARGB  0xFF101830u          /* what we expect to come out again  */
#define VTX_OFFSET  0x00010000u          /* RSX offset of our vertex buffer   */

/* Functions the RSX side exports but does not declare in a public header. */
extern void cellGcm_rsx_process_fifo(void);
extern int  cellGcm_take_flip_pending_synced(void);

/* --- guest memory helpers (all guest writes are big-endian) --------------- */

static void guest_w32(uint32_t addr, uint32_t v)
{
    uint8_t* p = vm_base + addr;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

static uint32_t g_heap = HEAP_BASE;

static u32 host_alloc(u32 size, u32 align)
{
    if (align == 0) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}

static void host_w32(u32 addr, u32 v) { guest_w32(addr, v); }

/* --- NV4097 command buffer ------------------------------------------------ */

static uint32_t g_fifo_len = 0;

/* One non-incrementing method write: [count<<18 | method] followed by data. */
static void emit(uint32_t method, uint32_t data)
{
    guest_w32(IO_ADDR + g_fifo_len, (1u << 18) | (((method >> 2) & 0x7FFu) << 2));
    g_fifo_len += 4;
    guest_w32(IO_ADDR + g_fifo_len, data);
    g_fifo_len += 4;
}

/* Write a big-endian float into the guest's IO window. */
static void guest_f32(uint32_t addr, float f)
{
    uint32_t w; memcpy(&w, &f, 4); guest_w32(addr, w);
}

/* A full-viewport triangle in clip space, vertex-coloured. With no vertex
 * program loaded the backend falls back to an identity transform, so these
 * coordinates land directly in NDC.
 *
 * Layout per vertex: float4 position at +0, float4 colour at +16, stride 32.
 * Position is attribute 0 and diffuse colour attribute 3, matching the slots
 * the backend's fallback path reads. */
static void upload_triangle(void)
{
    static const float v[3][8] = {
        { -1.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        {  3.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
    };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + VTX_OFFSET + (uint32_t)(i * 32 + k * 4), v[i][k]);
}

/* type[3:0]=2 (float32), size[7:4], stride[15:8] */
#define VFMT(size, stride) (2u | ((u32)(size) << 4) | ((u32)(stride) << 8))

static void emit_triangle_draw(void)
{
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_OFFSET +  0);  /* position */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_OFFSET + 16);  /* colour   */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES        */
    emit(NV4097_DRAW_ARRAYS,   0u | ((3u - 1u) << 24));   /* first=0 count=3  */
    emit(NV4097_SET_BEGIN_END, 0u);                       /* end              */
}

/* Append this frame's commands to the ring and advance `put`, exactly as a
 * title does. The RSX's `get` pointer lives inside cellGcmSys and chases `put`;
 * it is never rewound, so each frame must occupy fresh ring space rather than
 * overwrite the last one. */
static void submit_frame(int with_draw)
{
    emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
    emit(NV4097_CLEAR_SURFACE,         0xF0u);
    if (with_draw) emit_triangle_draw();

    guest_w32(CTRL_ADDR + 0, g_fifo_len);   /* put */
    cellGcm_rsx_process_fifo();
}

int main(int argc, char** argv)
{
    /* frames = 0 runs until the window is closed, which is what the .app
     * bundle uses; a fixed count keeps the CI runs bounded. */
    int frames = 3, do_draw = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frames=", 9) == 0) frames = atoi(argv[i] + 9);
        else if (strcmp(argv[i], "--draw") == 0)   do_draw = 1;
    }

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { fprintf(stderr, "[host] guest VM alloc failed\n"); return 1; }

    if (rsx_metal_backend_init(1280, 720, "ps3recomp") != 0) {
        fprintf(stderr, "[host] backend init failed\n");
        free(vm_base);
        return 1;
    }

    /* Bring up the GCM context exactly as a guest's cellGcmInit would. */
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata   = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR,
                                      host_alloc, host_w32);
    printf("[host] gcm context data @ 0x%08X\n", cdata);
    cellGcmSetDisplayBuffer(0, 0, 1280 * 4, 1280, 720);

    guest_w32(CTRL_ADDR + 4, 0);            /* get: start of ring */
    if (do_draw) upload_triangle();
    submit_frame(do_draw);

    u32 got = rsx_metal_backend_debug_color();
    printf("[host] clear colour through the FIFO: 0x%08X (expected 0x%08X) %s\n",
           got, CLEAR_ARGB, got == CLEAR_ARGB ? "OK" : "MISMATCH");

    /* Flip loop, as cellGcmSetFlipCommand + the vblank ticker drive it. */
    cellGcmSetFlipCommand(0);
    int presented = 0;
    for (int i = 0; frames == 0 || i < frames; i++) {
        if (rsx_metal_backend_pump_messages() < 0) { printf("[host] window closed\n"); break; }
        /* Re-submit every frame, as a title does: the backend records draws
         * per frame and clears the record when it presents. */
        if (i > 0) submit_frame(do_draw);
        cellGcmTickVBlank();
        cellGcmTickFlip();
        if (cellGcm_take_flip_pending_synced()) presented++;
        rsx_metal_backend_present();
    }
    printf("[host] presented %d frame(s)\n", presented);

    u32 back = rsx_metal_backend_readback_center();
    int rc = (got == CLEAR_ARGB) ? 0 : 2;
    if (back) {
        /* Headless: the drawable is ours, so verify what actually landed.
         * RSX ARGB -> Metal BGRA8Unorm keeps R, G and B in the same bytes. */
        /* Without a draw the centre pixel is the clear colour; with the test
         * triangle it is the triangle's red, which proves the vertex fetch,
         * primitive path, pipeline state and draw all worked. */
        u32 want = do_draw ? 0xFFFF0000u : CLEAR_ARGB;
        printf("[host] presented pixel: 0x%08X (expected 0x%08X) %s\n",
               back, want, (back & 0x00FFFFFFu) == (want & 0x00FFFFFFu) ? "OK" : "MISMATCH");
        if ((back & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) rc = 3;
    }

    rsx_metal_backend_shutdown();
    free(vm_base);
    return rc;
}
