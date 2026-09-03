/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * The first non-Windows render path in the project. It implements the subset
 * of the rsx_backend vtable needed to drive a guest clear/draw/flip loop:
 * `clear` captures the colour written by NV4097_CLEAR_SURFACE,
 * `set_render_target` tracks the guest's surface dimensions, the draw
 * callbacks record draws with their vertices fetched out of guest memory, and
 * present replays them into a drawable. Every dispatch site in rsx_commands.c
 * is guarded (`if (s_backend && s_backend->x)`), so a partial vtable is the
 * intended way to bring a backend up incrementally.
 *
 * Guest shaders. A draw runs the guest's own vertex and fragment programs
 * when both translate: the NV40 microcode goes through the RSX decompilers
 * (HLSL), then rsx_hlsl_to_msl (glslang + spirv-cross), then
 * -newLibraryWithSource:. What the D3D12 backend does with those programs is
 * mirrored here field for field -- where the vertex program starts, how the
 * fragment program is located and keyed, the viewport epilogue, the per-draw
 * constant blocks -- because that is the path real titles have exercised.
 * With no vertex program loaded, or with the translator compiled out, a draw
 * falls back to the built-in fixed-function shader, which is what the
 * clear/draw host checks use.
 *
 * Guest textures. A guest-program draw binds the enabled texture units the
 * way the D3D12 backend does: SET_TEXTURE_FORMAT's location bits say where
 * the bytes are, rsx_texture_layout says what shape they are,
 * rsx_texture_decode turns them into host rows, and the TEXTURE_CONTROL1
 * crossbar becomes the texture's swizzle. Level 0 only for now.
 *
 * Why Metal rather than SDL_Renderer: SDL2's renderer is a 2D sprite API with
 * no route to a custom vertex program, depth/stencil, MRT or render-to-texture,
 * so it cannot grow into the real pipeline.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#if !TARGET_OS_IPHONE
#  import <AppKit/AppKit.h>
#endif

#include "rsx_commands.h"
#include "rsx_metal_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_primitives.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_msl.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

static id<MTLDevice>       s_dev;
static id<MTLCommandQueue> s_queue;
static CAMetalLayer*       s_layer;      /* windowed only   */
static id<MTLTexture>      s_offscreen;  /* headless only   */
static id<MTLTexture>      s_depth;      /* depth + stencil, both surfaces  */
static NSWindow*           s_window;
static int                 s_headless;
static int                 s_closed;
static int                 s_ready;
static u32                 s_width  = 1280;
static u32                 s_height = 720;

/* ---- vertex layout ----------------------------------------------------------
 * Every draw carries all 16 RSX vertex attributes as float4, fetched on the
 * CPU by rsx_fetch_attrib: 256 bytes per vertex, attribute i at offset i*16.
 * That is the layout a decompiled vertex program expects (ATTRn arrives at
 * [[attribute(n)]]), and the built-in shader reads slots 0, 3 and 8 of the
 * same layout, so one staging buffer and one vertex descriptor serve both.
 * --------------------------------------------------------------------------*/

#define MTL_ATTRIBS        16
#define MTL_MAX_RECORDS    4096
#define MTL_MAX_VERTS      (256u * 1024u)
#define MTL_VB_INDEX       30      /* buffer(0) and buffer(1) are the constant blocks */

typedef struct { float a[MTL_ATTRIBS][4]; } MtlVertex;

/* ---- the frame's record stream ----------------------------------------------
 * RSX work arrives while the guest builds its frame; the flip comes later. So
 * draws are recorded with their vertices already fetched out of guest memory,
 * then replayed at present time. This mirrors the D3D12 backend's
 * D3D12DrawRecord / render_frame split.
 *
 * Clears are records in the same ordered stream, because a clear in the middle
 * of a frame is something a title really does -- it draws the world, clears
 * depth, and draws the first-person weapon over it. Replaying every draw into
 * one pass that clears once at the top loses that clear entirely. So a clear
 * record ends the open render pass and starts a new one whose flagged
 * attachments load-action Clear and whose others load-action Load.
 *
 * Render targets will hang off the same stream: a target change is one more
 * record that ends the pass and opens the next one somewhere else.
 * --------------------------------------------------------------------------*/

/* NV4097_CLEAR_SURFACE flags: 0x01 depth, 0x02 stencil, and one bit per colour
 * channel in 0xF0 (nouveau's NV30_3D_CLEAR_BUFFERS layout). A title clears the
 * four colour channels together, so 0xF0 is treated as one colour bit here,
 * exactly as the old clear callback did. */
#define MTL_CLEAR_DEPTH    0x01u
#define MTL_CLEAR_STENCIL  0x02u
#define MTL_CLEAR_COLOR    0xF0u

typedef struct {
    u32 base;        /* first vertex in s_verts               */
    u32 count;       /* vertex count after primitive expansion */
    MTLPrimitiveType topology;
    int blend_enable;
    u32 blend_sfactor, blend_dfactor, blend_equation;
    u32 color_mask;  /* NV4097_SET_COLOR_MASK word              */
    MTLCullMode cull;
    MTLWinding  winding;
    int ds_idx;      /* slot in s_ds_cache, or -1 for no test / no writes */
    u32 stencil_ref; /* SET_STENCIL_FUNC_REF; encoder state, not pipeline */
    /* Guest programs: cache slots, or -1 when the draw uses the built-in
     * shader. Slots are stable for the whole frame (see s_caches_full). */
    int vs_idx, fs_idx;
    u32 vp_cb_off;            /* VPConst block in s_cb                  */
    u32 fp_cb_off, fp_cb_len; /* PSConstants block in s_cb              */
    /* Per texture unit: texture and sampler cache slots, or -1 for the
     * zero texture / default sampler. Guest-program draws only. */
    int tex[RSX_MAX_TEXTURES];
    int samp[RSX_MAX_TEXTURES];
    float mvp[16];   /* built-in path: 4 rows of the RSX vertex-constant matrix */
} MtlDraw;

/* One NV4097_CLEAR_SURFACE, with the values rsx_commands.c decoded out of
 * SET_COLOR_CLEAR_VALUE and SET_ZSTENCIL_CLEAR_VALUE. */
typedef struct {
    u32   flags;     /* CLEAR_SURFACE mask                     */
    u32   color;     /* ARGB8888                               */
    float depth;     /* [0,1]                                  */
    u8    stencil;
} MtlClear;

typedef enum { MTL_REC_DRAW, MTL_REC_CLEAR } MtlRecordKind;

typedef struct {
    MtlRecordKind kind;
    union { MtlDraw draw; MtlClear clear; } u;
} MtlRecord;

static MtlVertex* s_verts;
static u32        s_vert_count;
static MtlRecord  s_records[MTL_MAX_RECORDS];
static u32        s_rec_count;
static u32        s_dropped_records;
static u32        s_guest_draws;       /* draws through guest programs this frame */
static u32        s_last_guest_draws;  /* ... in the last presented frame        */

static id<MTLLibrary> s_shader_lib;    /* built-in fixed-function shaders */

/* ---- per-draw constant blocks -----------------------------------------------
 * VPConst is the 512 float4 transform constants plus the viewport epilogue's
 * posscale/posoffset (8224 bytes); PSConstants is the fragment program's
 * inline constants plus fp_alpha. Constants change between draws in a frame,
 * so each draw snapshots its own blocks -- the D3D12 backend keeps a per-draw
 * VP_CB_STRIDE slot for the same reason. Both blocks are past the 4 KB limit
 * of setVertexBytes:, and Metal wants setVertexBuffer:offset: 256-aligned,
 * so they are appended here at record time and uploaded once per frame in
 * one buffer, exactly like the vertices.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CB_BYTES  ((RSX_MAX_VERTEX_CONSTANTS + 2) * 16)
#define MTL_CB_ALIGN     256u
#define MTL_CB_MAX       (256u << 20)
static u8* s_cb;
static u32 s_cb_used, s_cb_cap;

/* ---- guest shader caches ----------------------------------------------------
 * Vertex programs are keyed on their microcode bytes, fragment programs on
 * rsx_fp_structural_hash (the code, not the inline constants, which live in
 * PSConstants and change per draw) plus the export width and the alpha test
 * that gets patched into the source. A slot with a nil function is a program
 * that failed to translate: it is remembered so the draw does not retry the
 * whole pipeline on every frame.
 *
 * Draw records hold slot indices, so slots must not move while a frame is
 * being recorded. When a cache fills, translation stops for the rest of the
 * frame (those draws use the built-in shader) and every cache is emptied
 * after the frame presents, rather than evicting under a live record.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CACHE     1024
#define MTL_FP_CACHE     2048
#define MTL_PSO_CACHE    2048
#define MTL_FP_MAX_BYTES 4096u     /* bound on a fragment program, as D3D12 */

typedef struct { u32 hash; id<MTLFunction> fn; } MtlVpEntry;
typedef struct { u64 key;  id<MTLFunction> fn; u32 nconst; } MtlFpEntry;
typedef struct { int vs, fs; u32 blend; u32 cmask; } MtlPsoKey;
typedef struct { MtlPsoKey key; id<MTLRenderPipelineState> pso; } MtlPsoEntry;

static MtlVpEntry  s_vp_cache[MTL_VP_CACHE];
static MtlFpEntry  s_fp_cache[MTL_FP_CACHE];
static MtlPsoEntry s_pso_cache[MTL_PSO_CACHE];
static u32 s_vp_count, s_fp_count, s_pso_count;
static int s_caches_full;
static int s_guest_shaders;            /* translator present and not disabled */
static rsx_fp_constant_block s_fp_consts;

/* The decompilers' HLSL and the MSL made from it. The VP decompiler builds
 * bodies up to 192 KB, and the legalized MSL runs longer than its HLSL. */
static char s_hlsl[512 * 1024];
static char s_msl[512 * 1024];
static char s_log[8192];

/* What a fragment program samples before guest textures are bound (task:
 * textures). A D3D12 null SRV reads zero; Metal validation rejects a nil
 * texture on use, so zero is a real 1x1 texture here. */
static id<MTLTexture>      s_null_tex;
static id<MTLSamplerState> s_default_sampler;
/* What a draw gets when the depth/stencil cache is full: no test, no writes. */
static id<MTLDepthStencilState> s_ds_default;

/* ---- guest textures ---------------------------------------------------------
 * Keyed on where the bytes are and what the register says they are, plus a
 * sparse checksum of the bytes themselves, because titles animate textures
 * in place (the D3D12 backend's tex_csum exists for the same reason). A
 * changed checksum gets a fresh MTLTexture rather than an in-place write: a
 * frame in flight may still be sampling the old one. Same slot-stability
 * rule as the shader caches. Samplers are keyed on the wrap and filter
 * registers. Level 0 only; mip levels and cube faces are still to come.
 * --------------------------------------------------------------------------*/

#define MTL_TEX_CACHE   1024
#define MTL_SAMP_CACHE  64

typedef struct {
    u32 ea, w, h, format, control1, csum;
    id<MTLTexture> tex;
} MtlTexEntry;
typedef struct { u32 key; id<MTLSamplerState> samp; } MtlSampEntry;

static MtlTexEntry  s_tex_cache[MTL_TEX_CACHE];
static MtlSampEntry s_samp_cache[MTL_SAMP_CACHE];
static u32 s_tex_count, s_samp_count;
static u8* s_tex_staging;
static u32 s_tex_staging_cap;

/* Frames the CPU may run ahead of the GPU.
 *
 * Presenting used to end in -waitUntilCompleted unconditionally, which is the
 * one pattern where a translation layer or a driver cannot hide its encoding
 * work: the CPU stalls on every frame instead of preparing the next one.
 * Dolphin measured exactly this shape (their bounding-box path) as the single
 * largest CPU-side penalty in their Metal/MoltenVK comparison.
 *
 * Windowed presents now bound in-flight frames with a semaphore released from
 * the command buffer's completion handler, so the CPU keeps working while the
 * GPU drains. Headless still waits, because the readback has to observe a
 * finished frame -- there the stall is the point. */
#define MTL_MAX_INFLIGHT 3
static dispatch_semaphore_t s_inflight;

/* RSX clear colour, ARGB8888, as written by NV4097_SET_COLOR_CLEAR_VALUE. */
static u32 s_clear_argb = 0xFF000000u;
static u32 s_last_present_bgra;

/* ---- rsx_backend vtable -------------------------------------------------- */

static void mtl_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    /* The debug hook reports the last colour the guest asked for, whether or
     * not the record stream had room for the clear itself. */
    if (flags & MTL_CLEAR_COLOR) s_clear_argb = color;
    if (!s_ready) return;
    if (!(flags & (MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL))) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count++];
    r->kind = MTL_REC_CLEAR;
    r->u.clear.flags   = flags;
    r->u.clear.color   = color;
    r->u.clear.depth   = depth;
    r->u.clear.stencil = stencil;
}

/* The draw callbacks are not handed the state, so it is latched here. Every
 * state setter caches it; set_vertex_attribs in particular always fires before
 * a draw. The D3D12 backend does the same via s_d3d.current_rsx_state. */
static const rsx_state* s_state;

static int create_depth(void);

static void mtl_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    if (state) s_state = state;
    if (!state) return;
    u32 w = state->surface_clip_w, h = state->surface_clip_h;
    if (!w || !h || (w == s_width && h == s_height)) return;
    s_width = w; s_height = h;
    if (s_layer) s_layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
    if (s_depth && create_depth() != 0)
        fprintf(stderr, "[RSX metal] depth/stencil resize to %ux%u failed; "
                        "the old buffer stays bound\n", w, h);
}

static void mtl_latch_state(void* ud, const rsx_state* state)
{ (void)ud; if (state) s_state = state; }

static void mtl_draw_arrays(void*, u32, u32, u32);
static void mtl_draw_indexed(void*, u32, u32, u32);

static rsx_backend s_backend_vtable = {
    .userdata           = NULL,
    .clear              = mtl_clear,
    .set_render_target  = mtl_set_render_target,
    .set_vertex_attribs = mtl_latch_state,
    .set_blend          = mtl_latch_state,
    .set_depth_stencil  = mtl_latch_state,
    .set_viewport       = mtl_latch_state,
    .set_color_mask     = mtl_latch_state,
    .set_alpha_test     = mtl_latch_state,
    /* Programs are resolved when the draw is recorded, from the live state:
     * SET_SHADER_PROGRAM, the transform program words and SHADER_CONTROL
     * all land there before BEGIN_END fires this. */
    .set_shader         = mtl_latch_state,
    .draw_arrays        = mtl_draw_arrays,
    .draw_indexed       = mtl_draw_indexed,
};

/* ---- helpers ------------------------------------------------------------- */

static MTLClearColor clear_color_from_argb(u32 argb)
{
    /* ARGB8888 -> normalised RGBA. sRGB conversion is deliberately skipped:
     * the D3D12 backend treats the guest value as raw UNORM too, so both
     * backends agree pixel-for-pixel. */
    const double a = (double)((argb >> 24) & 0xFF) / 255.0;
    const double r = (double)((argb >> 16) & 0xFF) / 255.0;
    const double g = (double)((argb >>  8) & 0xFF) / 255.0;
    const double b = (double)( argb        & 0xFF) / 255.0;
    return MTLClearColorMake(r, g, b, a);
}

static u32 fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 fnv1a64(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* One depth/stencil texture for the display target, sized like the colour
 * attachment, shared by every pass the frame opens -- the D3D12 backend shares
 * one depth buffer the same way, because a PS3 title typically uses a single
 * zeta surface. Zeta offsets and per-target depth belong with render targets.
 *
 * D24S8 is not a format Apple GPUs have, so the depth is 32-bit float. Both
 * hold the guest's [0,1] window z, with more precision than the RSX's 24 bits
 * rather than less. The texture is never read back, so it is private.
 *
 * Assigned only on success: a failed resize keeps the old texture bound rather
 * than leaving passes and pipelines disagreeing about whether depth exists. */
#define MTL_DEPTH_FORMAT   MTLPixelFormatDepth32Float_Stencil8

static int create_depth(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:s_width
                                                          height:s_height
                                                       mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    if (!t) return -1;
    s_depth = t;
    return 0;
}

static int create_offscreen(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:s_width
                                                          height:s_height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_offscreen = [s_dev newTextureWithDescriptor:td];
    return s_offscreen ? 0 : -1;
}

#if !TARGET_OS_IPHONE
static int create_window(const char* title)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSRect frame = NSMakeRect(0, 0, (CGFloat)s_width, (CGFloat)s_height);
    s_window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!s_window) return -1;

    [s_window setTitle:[NSString stringWithUTF8String:(title ? title : "ps3recomp")]];
    [s_window center];

    s_layer = [CAMetalLayer layer];
    s_layer.device          = s_dev;
    s_layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = YES;
    s_layer.drawableSize    = CGSizeMake((CGFloat)s_width, (CGFloat)s_height);

    NSView* view = [s_window contentView];
    [view setWantsLayer:YES];
    [view setLayer:s_layer];

    [s_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return 0;
}
#endif

static int create_placeholders(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    s_null_tex = [s_dev newTextureWithDescriptor:td];
    if (!s_null_tex) return -1;
    u32 zero = 0;
    [s_null_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                    withBytes:&zero bytesPerRow:4];

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_default_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    if (!s_default_sampler) return -1;

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = NO;
    s_ds_default = [s_dev newDepthStencilStateWithDescriptor:dd];
    return s_ds_default ? 0 : -1;
}

/* ---- guest vertex fetch --------------------------------------------------
 * rsx_vertex_fetch.c holds the one definition of how a vertex comes out of
 * guest memory (this file used to carry a drifted copy). Every attribute is
 * fetched, used or not: a disabled array yields the constant attribute
 * register, which is what the hardware feeds a program that reads it.
 * --------------------------------------------------------------------------*/

static void fetch_vertex(const rsx_state* st, u32 vi, MtlVertex* out)
{
    for (int i = 0; i < MTL_ATTRIBS; i++)
        rsx_fetch_attrib(st, i, vi, out->a[i]);
}

/* ---- primitive conversion -------------------------------------------------
 * Metal, like D3D12, has no quads, quad strips, triangle fans or polygons, so
 * those are expanded to triangle lists on the CPU while the vertices are being
 * fetched. Everything else maps directly.
 * --------------------------------------------------------------------------*/

/* Which primitives need CPU expansion, and what they end up drawn as, both
 * come from rsx_primitives.h now -- this file used to answer the first
 * question with its own list, which disagreed with the one in that header
 * about LINE_LOOP and POLYGON. Only the Metal enum mapping stays here. */
static MTLPrimitiveType topo_to_metal(rsx_topology t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return MTLPrimitiveTypePoint;
    case RSX_TOPOLOGY_LINES:          return MTLPrimitiveTypeLine;
    case RSX_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default:                          return MTLPrimitiveTypeTriangle;
    }
}

/* Emit `count` vertices starting at `first`, expanding fans/quads/polygons.
 * `resolve` maps a sequence position to a guest vertex index, so the same code
 * serves both array and indexed draws. Returns vertices written. */
typedef u32 (*IndexResolver)(const rsx_state*, u32 /*seq*/, void* /*ctx*/);

static u32 emit_vertices(const rsx_state* st, u32 prim, u32 count,
                         IndexResolver resolve, void* ctx)
{
    u32 wrote = 0;
    #define PUSH(seq) do {                                                     \
        if (s_vert_count >= MTL_MAX_VERTS) return wrote;                       \
        fetch_vertex(st, resolve(st, (seq), ctx), &s_verts[s_vert_count++]);   \
        wrote++;                                                               \
    } while (0)

    if (prim == RSX_PRIMITIVE_QUADS) {
        for (u32 q = 0; q + 3 < count; q += 4) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q); PUSH(q+2); PUSH(q+3);
        }
    } else if (prim == RSX_PRIMITIVE_QUAD_STRIP) {
        for (u32 q = 0; q + 3 < count; q += 2) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q+1); PUSH(q+3); PUSH(q+2);
        }
    } else if (prim == RSX_PRIMITIVE_TRIANGLE_FAN || prim == RSX_PRIMITIVE_POLYGON) {
        for (u32 t = 1; t + 1 < count; t++) { PUSH(0); PUSH(t); PUSH(t+1); }
    } else {
        for (u32 v = 0; v < count; v++) PUSH(v);
    }
    #undef PUSH
    return wrote;
}

static u32 resolve_linear(const rsx_state* st, u32 seq, void* ctx)
{
    (void)st; return *(u32*)ctx + seq;
}

static u32 resolve_indexed(const rsx_state* st, u32 seq, void* ctx)
{
    u32 base = *(u32*)ctx;
    /* index_array_offset bits [7:4] select the type: 0 = u32, 1 = u16. */
    int u16type = ((st->index_array_offset >> 4) & 0xFu) == 1;
    u32 off = st->index_array_offset & ~0xFFu;
    u32 ea  = cellGcmResolveOffset(off) + (base + seq) * (u16type ? 2u : 4u);
    if (!vm_base) return 0;
    const u8* p = vm_base + ea;
    return u16type ? (u32)((p[0] << 8) | p[1])
                   : (u32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* ---- guest shader translation -------------------------------------------- */

/* PS3RECOMP_METAL_SHADER_DUMP=<dir>: write every translated program's HLSL and
 * MSL there, named by the cache key. The first thing to look at when a title
 * draws wrong. */
static void dump_shader(const char* name, const char* text)
{
    static const char* dir = (const char*)1;
    if (dir == (const char*)1) dir = getenv("PS3RECOMP_METAL_SHADER_DUMP");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static MTLCompileOptions* guest_compile_options(void)
{
    MTLCompileOptions* o = [MTLCompileOptions new];
    /* IEEE comparisons. The decompilers flush a NaN result to zero with
     * `x == x`, and the alpha test compares with isunordered; Metal's default
     * fast math is free to fold both away. */
#if defined(MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) {
        o.mathMode = MTLMathModeSafe;
        return o;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    return o;
}

/* MSL source -> its `main0` function, or nil with the compiler's diagnostic
 * on stderr. */
static id<MTLFunction> compile_guest_function(const char* msl, const char* what)
{
    NSError* err = nil;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:msl]
                                             options:guest_compile_options()
                                               error:&err];
    if (!lib) {
        fprintf(stderr, "[RSX metal] %s: MSL compile failed: %s\n", what,
                [[err localizedDescription] UTF8String]);
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    if (!fn) fprintf(stderr, "[RSX metal] %s: no main0 in the compiled MSL\n", what);
    return fn;
}

/* The vertex program the draw would run: a slot in s_vp_cache, or -1 when
 * there is none, it could not be translated, or the cache is full. Follows
 * the D3D12 backend's vp_get_vs: the program starts at the instruction
 * SET_TRANSFORM_PROGRAM_START selects (the store holds every resident
 * program), falling back to 0 when that lies past what was uploaded. */
static int vp_slot_for(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    /* Hash the program's own bytes, to its end bit, not everything after it
     * in the store: another program uploaded behind it must not miss. */
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = fnv1a32(uc, len);

    for (u32 i = 0; i < s_vp_count; i++)
        if (s_vp_cache[i].hash == hash) return s_vp_cache[i].fn ? (int)i : -1;
    if (s_vp_count >= MTL_VP_CACHE) { s_caches_full = 1; return -1; }

    char name[64];
    snprintf(name, sizeof name, "vertex program %08X", hash);
    id<MTLFunction> fn = nil;
    const int ni = rsx_vp_decompile(uc, len, s_hlsl, sizeof s_hlsl);
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] %s: decompile failed (%d)\n", name, ni);
    } else {
        snprintf(name, sizeof name, "vp_%08x.hlsl", hash); dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_VERTEX, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] vertex program %08X: %s\n", hash, s_log);
        } else {
            snprintf(name, sizeof name, "vp_%08x.msl", hash); dump_shader(name, s_msl);
            snprintf(name, sizeof name, "vertex program %08X", hash);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (n++ < 32)
        fprintf(stderr, "[RSX metal] vertex program %08X: %d instrs -> %s\n",
                hash, ni, fn ? "ok" : "FAILED (built-in shader instead)"); }

    const int slot = (int)s_vp_count++;
    s_vp_cache[slot].hash = hash;
    s_vp_cache[slot].fn   = fn;
    return fn ? slot : -1;
}

/* The fragment program the draw would run, resolved as the D3D12 backend's
 * vp_get_fp_pso does: SET_SHADER_PROGRAM's low bits are the location (1 =
 * local, 2 = main), the rest the offset. Keyed on the program's structure
 * (its inline constants are hoisted into PSConstants, so the compiled shader
 * is invariant under constant changes), the export width from
 * SHADER_CONTROL, and the alpha test patched into the source. Returns the
 * slot or -1; `*uc` receives the program bytes for the constant collection. */
static int fp_slot_for(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;

    u64 key = rsx_fp_structural_hash(*uc, MTL_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                    /* malformed or unterminated */
    const u32 ctrl      = st->shader_control;
    const u32 ctrl_key  = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en  = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn  = alpha_en ? st->alpha_func : 0u;
    key = fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = fnv1a64(&alpha_fn, sizeof alpha_fn, key);

    for (u32 i = 0; i < s_fp_count; i++)
        if (s_fp_cache[i].key == key) return s_fp_cache[i].fn ? (int)i : -1;
    if (s_fp_count >= MTL_FP_CACHE) { s_caches_full = 1; return -1; }

    char name[64];
    id<MTLFunction> fn = nil;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, MTL_FP_MAX_BYTES, ctrl, 0 /* all 2D */,
                                          s_hlsl, sizeof s_hlsl, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_hlsl, sizeof s_hlsl, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] fragment program %016llX: decompile failed (%d)\n",
                (unsigned long long)key, ni);
    } else {
        snprintf(name, sizeof name, "fp_%016llx.hlsl", (unsigned long long)key);
        dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_FRAGMENT, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] fragment program %016llX: %s\n",
                    (unsigned long long)key, s_log);
        } else {
            snprintf(name, sizeof name, "fp_%016llx.msl", (unsigned long long)key);
            dump_shader(name, s_msl);
            snprintf(name, sizeof name, "fragment program %016llX", (unsigned long long)key);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (n++ < 32)
        fprintf(stderr, "[RSX metal] fragment program %016llX: %d instrs, %u constants -> %s\n",
                (unsigned long long)key, ni, nconst,
                fn ? "ok" : "FAILED (built-in shader instead)"); }

    const int slot = (int)s_fp_count++;
    s_fp_cache[slot].key    = key;
    s_fp_cache[slot].fn     = fn;
    s_fp_cache[slot].nconst = nconst;
    return fn ? slot : -1;
}

/* Reserve `bytes` of constant staging at a 256-byte boundary. */
static int cb_alloc(u32 bytes, u32* off)
{
    const u32 start = (s_cb_used + MTL_CB_ALIGN - 1u) & ~(MTL_CB_ALIGN - 1u);
    if (start + bytes > s_cb_cap) {
        u32 cap = s_cb_cap ? s_cb_cap : (4u << 20);
        while (start + bytes > cap) {
            if (cap >= MTL_CB_MAX) return 0;
            cap *= 2u;
        }
        u8* n = (u8*)realloc(s_cb, cap);
        if (!n) return 0;
        s_cb = n; s_cb_cap = cap;
    }
    *off = start;
    s_cb_used = start + bytes;
    return 1;
}

/* Resolve both guest programs for a draw and snapshot their constant blocks.
 * Returns 1 with the record's shader fields filled in, 0 when the draw must
 * use the built-in shader (no vertex program loaded, a translation failure,
 * or a full cache). Both programs or neither: a guest vertex program's
 * outputs do not link with the built-in fragment shader's inputs. */
static int guest_programs_for(const rsx_state* st, MtlDraw* d)
{
    const int vs = vp_slot_for(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = fp_slot_for(st, &fp_uc);
    if (fs < 0) return 0;

    /* PSConstants: the program's inline constants as host-order bit patterns,
     * one float4 per slot, then fp_alpha. The count must match what the
     * shader was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, MTL_FP_MAX_BYTES, &s_fp_consts) < 0 ||
        s_fp_consts.count != s_fp_cache[fs].nconst)
        return 0;
    const u32 nslots = s_fp_consts.count ? s_fp_consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;

    u32 vp_off, fp_off;
    if (!cb_alloc(MTL_VP_CB_BYTES, &vp_off)) return 0;
    if (!cb_alloc(fp_len, &fp_off)) return 0;

    /* VPConst: the 512 transform constants, then the viewport epilogue
     * (see the D3D12 backend's vp_record_cb): x/y identity, and the z lane
     * remaps GL clip z to [0,1] when the guest programmed a z scale. Metal
     * clip space matches D3D's here, so the mapping transfers unchanged. */
    u8* vp = s_cb + vp_off;
    memcpy(vp, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    float* vpx = (float*)(vp + RSX_MAX_VERTEX_CONSTANTS * 16);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }

    u8* fp = s_cb + fp_off;
    memset(fp, 0, fp_len);
    if (s_fp_consts.count)
        memcpy(fp, s_fp_consts.values, s_fp_consts.count * 16u);
    float* alpha = (float*)(fp + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);
    alpha[1] = alpha[2] = alpha[3] = 0.0f;

    d->vs_idx = vs; d->fs_idx = fs;
    d->vp_cb_off = vp_off;
    d->fp_cb_off = fp_off; d->fp_cb_len = fp_len;
    return 1;
}

/* ---- guest textures ------------------------------------------------------ */

static MTLPixelFormat texfmt_to_metal(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8G8:     return MTLPixelFormatRG8Unorm;
    case RSX_TEXFMT_R8G8B8A8: return MTLPixelFormatRGBA8Unorm;
    case RSX_TEXFMT_BC1:      return MTLPixelFormatBC1_RGBA;
    case RSX_TEXFMT_BC2:      return MTLPixelFormatBC2_RGBA;
    case RSX_TEXFMT_BC3:      return MTLPixelFormatBC3_RGBA;
    case RSX_TEXFMT_R16:      return MTLPixelFormatR16Unorm;
    case RSX_TEXFMT_R16G16:   return MTLPixelFormatRG16Unorm;
    case RSX_TEXFMT_R16G16F:  return MTLPixelFormatRG16Float;
    case RSX_TEXFMT_R16G16B16A16F: return MTLPixelFormatRGBA16Float;
    case RSX_TEXFMT_R32F:     return MTLPixelFormatR32Float;
    case RSX_TEXFMT_R32G32B32A32F: return MTLPixelFormatRGBA32Float;
    default:                  return MTLPixelFormatR8Unorm;
    }
}

/* A crossbar selector (0..3 = the uploaded R,G,B,A; RSX_REMAP_ZERO/ONE) as
 * a Metal swizzle source. */
static MTLTextureSwizzle swizzle_sel(u8 sel)
{
    switch (sel) {
    case 0: return MTLTextureSwizzleRed;
    case 1: return MTLTextureSwizzleGreen;
    case 2: return MTLTextureSwizzleBlue;
    case 3: return MTLTextureSwizzleAlpha;
    case RSX_REMAP_ONE: return MTLTextureSwizzleOne;
    default: return MTLTextureSwizzleZero;
    }
}

/* Sparse FNV-1a over a texture's source bytes: enough to notice an animated
 * surface changing, cheap enough to run on every draw that binds it. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    const u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* Decode one level-0 image out of guest memory into a new texture whose
 * swizzle applies the TEXTURE_CONTROL1 crossbar. `fmt` is the format byte
 * with its LN/UN flags. */
static id<MTLTexture> upload_texture(const u8* src, u32 w, u32 h, u32 fmt, u32 control1)
{
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    if (tl.face_bytes == 0) return nil;
    /* Staging is sized by the DECODED image, which is wider than the source
     * for a format the host has to be handed unpacked. */
    const u32 staged = tl.dst_row_bytes * tl.rows;
    if (s_tex_staging_cap < staged) {
        u8* n = (u8*)realloc(s_tex_staging, staged);
        if (!n) return nil;
        s_tex_staging = n; s_tex_staging_cap = staged;
    }
    /* TEX_RGBA is consulted unconditionally, as the D3D12 backend does: the
     * decode only reads it for the two formats whose bytes are A,R,G,B. */
    rsx_texture_decode(s_tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:texfmt_to_metal(tl.fmt)
                                                           width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    /* The crossbar's selectors arrive in its own field order A,R,G,B; the
     * D3D12 backend maps them to destR = out[1], destG = out[2],
     * destB = out[3], destA = out[0], and so does this. */
    u8 remap[4];
    rsx_texture_component_remap(control1, fmt & 0x9Fu, remap);
    td.swizzle = MTLTextureSwizzleChannelsMake(swizzle_sel(remap[1]), swizzle_sel(remap[2]),
                                               swizzle_sel(remap[3]), swizzle_sel(remap[0]));
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    if (!tex) return nil;
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
             withBytes:s_tex_staging bytesPerRow:tl.dst_row_bytes];
    return tex;
}

/* The texture a unit samples: a slot in s_tex_cache, or -1 for the zero
 * texture (unit disabled, unresolvable, or a full cache). Resolved as the
 * D3D12 backend's bind_texture does it: the location is SET_TEXTURE_FORMAT's
 * low two bits (1 = local, 2 = main), the format byte its bits [15:8]. */
static int tex_slot_for(const rsx_texture_state* t)
{
    if (!(t->control0 & 0x80000000u) || !vm_base) return -1;
    const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) return -1;
    const u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (ea == 0xFFFFFFFFu) return -1;
    const u32 fmt = (t->format >> 8) & 0xFFu;
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    const u32 csum = tex_csum(vm_base + ea, tl.face_bytes);

    for (u32 i = 0; i < s_tex_count; i++) {
        MtlTexEntry* e = &s_tex_cache[i];
        if (e->ea != ea || e->w != w || e->h != h || e->format != t->format ||
            e->control1 != t->control1)
            continue;
        if (e->csum != csum) {
            id<MTLTexture> fresh = upload_texture(vm_base + ea, w, h, fmt, t->control1);
            if (!fresh) return -1;
            e->tex = fresh; e->csum = csum;
        }
        return e->tex ? (int)i : -1;
    }
    if (s_tex_count >= MTL_TEX_CACHE) { s_caches_full = 1; return -1; }

    id<MTLTexture> tex = upload_texture(vm_base + ea, w, h, fmt, t->control1);
    { static int n = 0; if (n++ < 16)
        fprintf(stderr, "[RSX metal] texture %ux%u fmt 0x%02X at 0x%08X (%s) -> %s\n",
                w, h, fmt, ea, (t->format & 3u) == 1u ? "local" : "main",
                tex ? "ok" : "FAILED"); }
    const int slot = (int)s_tex_count++;
    MtlTexEntry* e = &s_tex_cache[slot];
    e->ea = ea; e->w = w; e->h = h; e->format = t->format; e->control1 = t->control1;
    e->csum = csum; e->tex = tex;
    return tex ? slot : -1;
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field (one per axis): 1 = WRAP, 2 =
 * MIRROR, 3 = CLAMP_TO_EDGE, 4 = BORDER, 5 = CLAMP, 6..8 = the MIRROR_ONCE
 * family. The same table the live draw engine's gcm_wrap uses. */
static MTLSamplerAddressMode gcm_wrap_to_metal(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return MTLSamplerAddressModeRepeat;
    case 2:  return MTLSamplerAddressModeMirrorRepeat;
    case 4:  return MTLSamplerAddressModeClampToBorderColor;
    case 6: case 7: case 8:
             return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;   /* 3, 5, unset */
    }
}

/* Sampler from the unit's wrap and filter registers, as the live draw
 * engine's decode_sampler reads them: SET_TEXTURE_FILTER min at [18:16]
 * (1 NEAREST, 2 LINEAR, 3..6 with a mip filter), mag at [26:24]. One
 * level is uploaded, so the mip filter is left off. */
static int samp_slot_for(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 key = minf | (magf << 3) | ((t->address & 0x000F0F0Fu) << 6);
    for (u32 i = 0; i < s_samp_count; i++)
        if (s_samp_cache[i].key == key) return (int)i;
    if (s_samp_count >= MTL_SAMP_CACHE) return -1;

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = (minf == 2 || minf == 4 || minf == 6) ? MTLSamplerMinMagFilterLinear
                                                         : MTLSamplerMinMagFilterNearest;
    sd.magFilter = (magf == 2) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = gcm_wrap_to_metal(t->address);
    sd.tAddressMode = gcm_wrap_to_metal(t->address >> 8);
    sd.rAddressMode = gcm_wrap_to_metal(t->address >> 16);
    sd.borderColor  = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> samp = [s_dev newSamplerStateWithDescriptor:sd];
    if (!samp) return -1;
    const int slot = (int)s_samp_count++;
    s_samp_cache[slot].key  = key;
    s_samp_cache[slot].samp = samp;
    return slot;
}

/* Resolve every enabled texture unit for a guest-program draw. */
static void textures_for(const rsx_state* st, MtlDraw* d)
{
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        if (!(t->control0 & 0x80000000u)) continue;
        d->tex[u]  = tex_slot_for(t);
        d->samp[u] = samp_slot_for(t);
    }
}

/* Guest face culling, packed as the D3D12 backend's rsx_cull_key reads it:
 * CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408 (FRONT here, as
 * there); FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face and
 * front_face with plain 1/0 before any register arrives, which reads as
 * BACK/CW. Metal's default winding is clockwise, so the mapping is direct. */
static void cull_from_state(const rsx_state* st, MtlDraw* d)
{
    d->cull = MTLCullModeNone;
    if (st->cull_face_enable)
        d->cull = (st->cull_face == 0x0404u || st->cull_face == 0x0408u)
                      ? MTLCullModeFront : MTLCullModeBack;
    d->winding = (st->front_face == 0x0901u) ? MTLWindingCounterClockwise
                                             : MTLWindingClockwise;
}

/* ---- depth/stencil state -------------------------------------------------
 * The NV4097 comparison functions are the GL enums, 0x200 NEVER .. 0x207
 * ALWAYS, and the stencil ops are GL's too. Both tables are copied from the
 * live draw engine's gcm_cmp / gcm_stencil_op, which is what a title has
 * actually run through.
 *
 * Note that rsx_state_init seeds depth_func with 1 and calls it LESS, which is
 * not the GL enum for anything; like the reference, an unrecognised function
 * decodes as ALWAYS. Nothing reaches the depth test with that value unless the
 * guest enabled the test without programming SET_DEPTH_FUNC.
 * --------------------------------------------------------------------------*/

static MTLCompareFunction gcm_compare_to_metal(u32 f)
{
    switch (f) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    default:     return MTLCompareFunctionAlways;
    }
}

/* GL_ZERO is 0, which is also what the register reads before a title ever
 * writes it -- and GL_KEEP is the value that leaves such a stream alone, so
 * zero maps to ZERO only because the guest asked for it explicitly; an
 * unrecognised op keeps, as in the reference. */
static MTLStencilOperation gcm_stencil_op_to_metal(u32 op)
{
    switch (op) {
    case 0x0000: return MTLStencilOperationZero;              /* GL_ZERO       */
    case 0x1E01: return MTLStencilOperationReplace;
    case 0x1E02: return MTLStencilOperationIncrementClamp;    /* GL_INCR       */
    case 0x1E03: return MTLStencilOperationDecrementClamp;    /* GL_DECR       */
    case 0x150A: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    default:     return MTLStencilOperationKeep;              /* GL_KEEP 0x1E00 */
    }
}

/* MTLDepthStencilState objects are immutable, so one is built per distinct
 * combination of the registers and kept for the frame, exactly like the
 * pipeline states. The stencil reference value is not part of the object --
 * Metal sets it on the encoder -- so it is not part of the key either. */
#define MTL_DS_CACHE 256

typedef struct {
    u8  depth_test, depth_mask, stencil_test;
    u32 depth_func;
    u32 stencil_func, stencil_mask;
    u32 op_fail, op_zfail, op_zpass;
} MtlDsKey;
typedef struct { MtlDsKey key; id<MTLDepthStencilState> state; } MtlDsEntry;

static MtlDsEntry s_ds_cache[MTL_DS_CACHE];
static u32        s_ds_count;

static int ds_slot_for(const rsx_state* st)
{
    MtlDsKey key;
    memset(&key, 0, sizeof key);
    key.depth_test   = (u8)(st->depth_test_enable ? 1 : 0);
    key.depth_mask   = (u8)(st->depth_mask ? 1 : 0);
    key.stencil_test = (u8)(st->stencil_test_enable ? 1 : 0);
    key.depth_func   = st->depth_func;
    key.stencil_func = st->stencil_func;
    key.stencil_mask = st->stencil_mask;
    key.op_fail      = st->stencil_op_fail;
    key.op_zfail     = st->stencil_op_zfail;
    key.op_zpass     = st->stencil_op_zpass;

    for (u32 i = 0; i < s_ds_count; i++)
        if (memcmp(&s_ds_cache[i].key, &key, sizeof key) == 0) return (int)i;
    if (s_ds_count >= MTL_DS_CACHE) { s_caches_full = 1; return -1; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    /* The attachment is always bound, so "depth test off" is compare Always
     * with writes off -- which is also what the hardware does: with the test
     * disabled the RSX writes no depth, whatever SET_DEPTH_MASK says. */
    dd.depthCompareFunction = key.depth_test ? gcm_compare_to_metal(key.depth_func)
                                             : MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = (key.depth_test && key.depth_mask) ? YES : NO;
    if (key.stencil_test) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction    = gcm_compare_to_metal(key.stencil_func);
        sd.stencilFailureOperation   = gcm_stencil_op_to_metal(key.op_fail);
        sd.depthFailureOperation     = gcm_stencil_op_to_metal(key.op_zfail);
        sd.depthStencilPassOperation = gcm_stencil_op_to_metal(key.op_zpass);
        sd.readMask                  = key.stencil_mask & 0xFFu;
        /* SET_STENCIL_FUNC_MASK is the read mask. The write mask lives in
         * NV4097_SET_STENCIL_MASK (0x032C), which rsx_commands.c does not
         * decode and rsx_state has no field for, so every plane is writable
         * here. It reads back wrong for a title that masks stencil planes;
         * fixing it means adding the register upstream, not guessing here. */
        sd.writeMask                 = 0xFFu;
        /* Two-sided stencil (SET_BACK_STENCIL_*) is not decoded upstream
         * either; nv40 applies the front state to both faces when it is off,
         * which is what this does. */
        dd.frontFaceStencil = sd;
        dd.backFaceStencil  = sd;
    } else {
        dd.frontFaceStencil = nil;
        dd.backFaceStencil  = nil;
    }

    id<MTLDepthStencilState> state = [s_dev newDepthStencilStateWithDescriptor:dd];
    if (!state) return -1;
    const int slot = (int)s_ds_count++;
    s_ds_cache[slot].key   = key;
    s_ds_cache[slot].state = state;
    return slot;
}

/* ---- draw recording ------------------------------------------------------ */

static void record_draw(const rsx_state* st, u32 prim, u32 base, u32 count,
                        IndexResolver resolve)
{
    if (!s_ready || !s_verts || !st || count == 0) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count];
    r->kind = MTL_REC_DRAW;
    MtlDraw* d = &r->u.draw;
    memset(d, 0, sizeof *d);
    d->vs_idx = d->fs_idx = d->ds_idx = -1;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) d->tex[u] = d->samp[u] = -1;

    /* Programs first: their translation is what can fail, and a draw that
     * drops to the built-in shader must still fetch its vertices the same
     * way. Constant staging reserved here for a draw that then fails on
     * vertices is simply left unused until the frame resets it. */
    const int guest = s_guest_shaders && guest_programs_for(st, d);
    if (guest) textures_for(st, d);

    u32 first_vert = s_vert_count;
    u32 wrote = emit_vertices(st, prim, count, resolve, &base);
    if (wrote == 0) { s_dropped_records++; return; }
    s_rec_count++;
    if (guest) s_guest_draws++;

    d->base  = first_vert;
    d->count = wrote;
    /* Expanded primitives always come out as a triangle list. */
    d->topology = rsx_primitive_needs_expansion(prim) ? MTLPrimitiveTypeTriangle
                                             : topo_to_metal(rsx_primitive_topology(prim));
    d->blend_enable   = st->blend_enable;
    d->blend_sfactor  = st->blend_sfactor;
    d->blend_dfactor  = st->blend_dfactor;
    d->blend_equation = st->blend_equation;
    d->color_mask     = st->color_mask;
    d->ds_idx         = ds_slot_for(st);
    d->stencil_ref    = st->stencil_ref;
    cull_from_state(st, d);

    /* Built-in path: RSX vertex constant slots 0..3 hold the MVP rows when no
     * vertex program has been translated. An all-zero matrix would collapse
     * every vertex to the origin, so fall back to identity. */
    int nonzero = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = st->vertex_constants[r][c];
            d->mvp[r * 4 + c] = v;
            if (v != 0.0f) nonzero = 1;
        }
    if (!nonzero) {
        memset(d->mvp, 0, sizeof(d->mvp));
        d->mvp[0] = d->mvp[5] = d->mvp[10] = d->mvp[15] = 1.0f;
    }
}

static void mtl_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, first, count, resolve_linear);
}

static void mtl_draw_indexed(void* ud, u32 primitive, u32 index_offset, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, index_offset, count, resolve_indexed);
}

/* ---- pipeline state ------------------------------------------------------ */

static MTLBlendFactor blend_factor_to_metal(u32 f)
{
    switch (f) {
    case 0x0000: return MTLBlendFactorZero;
    case 0x0001: return MTLBlendFactorOne;
    case 0x0300: return MTLBlendFactorSourceColor;
    case 0x0301: return MTLBlendFactorOneMinusSourceColor;
    case 0x0302: return MTLBlendFactorSourceAlpha;
    case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x0304: return MTLBlendFactorDestinationAlpha;
    case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x0306: return MTLBlendFactorDestinationColor;
    case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
    default:     return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_equation_to_metal(u32 e)
{
    switch (e) {
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    case 0x800A: return MTLBlendOperationSubtract;
    case 0x800B: return MTLBlendOperationReverseSubtract;
    default:     return MTLBlendOperationAdd;
    }
}

/* NV4097_SET_COLOR_MASK: bit 0 = B, 8 = G, 16 = R, 24 = A. */
static MTLColorWriteMask color_mask_to_metal(u32 m)
{
    MTLColorWriteMask w = MTLColorWriteMaskNone;
    if (m & 0x00010000u) w |= MTLColorWriteMaskRed;
    if (m & 0x00000100u) w |= MTLColorWriteMaskGreen;
    if (m & 0x00000001u) w |= MTLColorWriteMaskBlue;
    if (m & 0x01000000u) w |= MTLColorWriteMaskAlpha;
    return w;
}

/* Built-in shaders: transform by the RSX vertex-constant matrix and interpolate
 * the diffuse colour. The rows are dotted explicitly rather than using float4x4
 * so there is no column-major/row-major ambiguity with the guest's layout.
 * Reads position, diffuse colour and texcoord0 from RSX attribute slots 0, 3
 * and 8 of the shared vertex layout. The guest-shader path replaces this, it
 * does not extend it. */
static NSString* const kBuiltinMSL = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn  { float4 pos [[attribute(0)]]; float4 col [[attribute(3)]]; float4 tc [[attribute(8)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 tc; };\n"
"struct VU   { float4 mvp[4]; };\n"
"vertex VOut vs_main(VIn v [[stage_in]], constant VU& u [[buffer(0)]]) {\n"
"    VOut o;\n"
"    o.pos = float4(dot(u.mvp[0], v.pos), dot(u.mvp[1], v.pos),\n"
"                   dot(u.mvp[2], v.pos), dot(u.mvp[3], v.pos));\n"
"    o.col = v.col; o.tc = v.tc;\n"
"    return o;\n"
"}\n"
"fragment float4 fs_main(VOut in [[stage_in]]) { return in.col; }\n";

static u32 blend_key(const MtlDraw* d)
{
    if (!d->blend_enable) return 0u;
    return 1u | ((d->blend_sfactor & 0xFFFu) << 1)
              | ((d->blend_dfactor & 0xFFFu) << 13)
              | ((d->blend_equation & 0x7u) << 25);
}

/* All 16 RSX attributes as float4 at i*16, stride 256, from MTL_VB_INDEX.
 * A guest program declares only the attributes it reads; Metal ignores the
 * rest of the descriptor. */
static MTLVertexDescriptor* vertex_descriptor(void)
{
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < MTL_ATTRIBS; i++) {
        vd.attributes[i].format      = MTLVertexFormatFloat4;
        vd.attributes[i].offset      = (NSUInteger)(i * 16);
        vd.attributes[i].bufferIndex = MTL_VB_INDEX;
    }
    vd.layouts[MTL_VB_INDEX].stride = sizeof(MtlVertex);
    return vd;
}

static id<MTLRenderPipelineState> pso_for(const MtlDraw* d)
{
    MtlPsoKey key;
    memset(&key, 0, sizeof key);
    key.vs = d->vs_idx; key.fs = d->fs_idx;
    key.blend = blend_key(d); key.cmask = d->color_mask;
    for (u32 i = 0; i < s_pso_count; i++)
        if (memcmp(&s_pso_cache[i].key, &key, sizeof key) == 0) return s_pso_cache[i].pso;
    if (s_pso_count >= MTL_PSO_CACHE) { s_caches_full = 1; return nil; }

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    if (d->vs_idx >= 0) {
        pd.vertexFunction   = s_vp_cache[d->vs_idx].fn;
        pd.fragmentFunction = s_fp_cache[d->fs_idx].fn;
    } else {
        pd.vertexFunction   = [s_shader_lib newFunctionWithName:@"vs_main"];
        pd.fragmentFunction = [s_shader_lib newFunctionWithName:@"fs_main"];
    }
    pd.vertexDescriptor = vertex_descriptor();
    /* The depth/stencil attachment is bound on every pass, so every pipeline
     * declares it; whether a draw tests or writes is the MTLDepthStencilState's
     * business, not the pipeline's. */
    pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
    pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.writeMask   = color_mask_to_metal(d->color_mask);
    if (d->blend_enable) {
        ca.blendingEnabled             = YES;
        ca.sourceRGBBlendFactor        = blend_factor_to_metal(d->blend_sfactor);
        ca.destinationRGBBlendFactor   = blend_factor_to_metal(d->blend_dfactor);
        ca.sourceAlphaBlendFactor      = blend_factor_to_metal(d->blend_sfactor);
        ca.destinationAlphaBlendFactor = blend_factor_to_metal(d->blend_dfactor);
        ca.rgbBlendOperation           = blend_equation_to_metal(d->blend_equation);
        ca.alphaBlendOperation         = blend_equation_to_metal(d->blend_equation);
    }

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
        fprintf(stderr, "[RSX metal] pipeline state failed (vs %d, fs %d): %s\n",
                d->vs_idx, d->fs_idx, [[err localizedDescription] UTF8String]);
        return nil;
    }
    s_pso_cache[s_pso_count].key = key;
    s_pso_cache[s_pso_count].pso = pso;
    s_pso_count++;
    return pso;
}

/* ---- replay --------------------------------------------------------------
 * Everything a render pass has to bind before its first draw. Encoder state
 * does not outlive endEncoding, so a clear in the middle of a frame pays for
 * this again on the pass it opens.
 * --------------------------------------------------------------------------*/

typedef struct {
    id<MTLBuffer> vb;
    id<MTLBuffer> cbuf;
    int tex[RSX_MAX_TEXTURES];    /* what each unit currently holds, or -1 */
    int samp[RSX_MAX_TEXTURES];
} MtlPassState;

/* Open a render pass on `color`. The attachments `clear` flags name load-action
 * Clear with its values; the rest load-action Load, so what an earlier pass in
 * the same frame left behind survives. */
static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb,
                                              id<MTLTexture> color,
                                              const MtlClear* clear,
                                              MtlPassState* ps)
{
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = color;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear->flags & MTL_CLEAR_COLOR) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = clear_color_from_argb(clear->color);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    /* Both halves of the one Depth32Float_Stencil8 texture. Stored rather than
     * discarded: a later pass in the same frame loads what this one wrote. */
    rp.depthAttachment.texture       = s_depth;
    rp.depthAttachment.storeAction   = MTLStoreActionStore;
    rp.depthAttachment.loadAction    = (clear->flags & MTL_CLEAR_DEPTH)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth    = (double)clear->depth;
    rp.stencilAttachment.texture     = s_depth;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.loadAction  = (clear->flags & MTL_CLEAR_STENCIL)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.stencilAttachment.clearStencil = clear->stencil;

    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) return nil;

    if (ps->vb) [enc setVertexBuffer:ps->vb offset:0 atIndex:MTL_VB_INDEX];
    /* Every fragment texture unit starts on the zero texture -- a guest
     * program may sample a unit nothing was bound to, and Metal validation
     * rejects a nil texture on use. Units are then rebound per draw only where
     * the draw's slot differs. */
    for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
        [enc setFragmentTexture:s_null_tex atIndex:(NSUInteger)u];
        [enc setFragmentSamplerState:s_default_sampler atIndex:(NSUInteger)u];
        ps->tex[u] = ps->samp[u] = -1;
    }
    MTLViewport vp = { 0.0, 0.0, (double)s_width, (double)s_height, 0.0, 1.0 };
    [enc setViewport:vp];
    return enc;
}

static void encode_draw(id<MTLRenderCommandEncoder> enc, const MtlDraw* d,
                        MtlPassState* ps)
{
    id<MTLRenderPipelineState> pso = pso_for(d);
    if (!pso) return;
    [enc setRenderPipelineState:pso];
    [enc setCullMode:d->cull];
    [enc setFrontFacingWinding:d->winding];
    [enc setDepthStencilState:(d->ds_idx >= 0 ? s_ds_cache[d->ds_idx].state
                                              : s_ds_default)];
    [enc setStencilReferenceValue:d->stencil_ref];
    if (d->vs_idx >= 0 && ps->cbuf) {
        [enc setVertexBuffer:ps->cbuf offset:d->vp_cb_off atIndex:0];
        [enc setFragmentBuffer:ps->cbuf offset:d->fp_cb_off atIndex:1];
        for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
            if (d->tex[u] != ps->tex[u]) {
                [enc setFragmentTexture:(d->tex[u] >= 0 ? s_tex_cache[d->tex[u]].tex
                                                        : s_null_tex)
                                atIndex:(NSUInteger)u];
                ps->tex[u] = d->tex[u];
            }
            if (d->samp[u] != ps->samp[u]) {
                [enc setFragmentSamplerState:(d->samp[u] >= 0 ? s_samp_cache[d->samp[u]].samp
                                                              : s_default_sampler)
                                     atIndex:(NSUInteger)u];
                ps->samp[u] = d->samp[u];
            }
        }
    } else {
        [enc setVertexBytes:d->mvp length:sizeof(d->mvp) atIndex:0];
    }
    [enc drawPrimitives:d->topology vertexStart:d->base vertexCount:d->count];
}

static void clear_caches(void)
{
    for (u32 i = 0; i < s_vp_count;   i++) s_vp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_fp_count;   i++) s_fp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_pso_count;  i++) s_pso_cache[i].pso   = nil;
    for (u32 i = 0; i < s_tex_count;  i++) s_tex_cache[i].tex   = nil;
    for (u32 i = 0; i < s_samp_count; i++) s_samp_cache[i].samp = nil;
    for (u32 i = 0; i < s_ds_count;   i++) s_ds_cache[i].state  = nil;
    s_vp_count = s_fp_count = s_pso_count = s_tex_count = s_samp_count = 0;
    s_ds_count = 0;
    s_caches_full = 0;
}

/* ---- public API ---------------------------------------------------------- */

int rsx_metal_backend_init(u32 width, u32 height, const char* title)
{
    @autoreleasepool {
        if (width)  s_width  = width;
        if (height) s_height = height;

        const char* hl = getenv("PS3RECOMP_METAL_HEADLESS");
        s_headless = (hl && *hl && *hl != '0');

        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) {
            fprintf(stderr, "[RSX metal] no Metal device available\n");
            return -1;
        }
        s_queue = [s_dev newCommandQueue];
        if (!s_queue) {
            fprintf(stderr, "[RSX metal] could not create command queue\n");
            return -1;
        }

        int rc;
#if TARGET_OS_IPHONE
        s_headless = 1;
        rc = create_offscreen();
#else
        rc = s_headless ? create_offscreen() : create_window(title);
#endif
        if (rc != 0) {
            fprintf(stderr, "[RSX metal] surface creation failed\n");
            return -1;
        }
        if (create_depth() != 0) {
            fprintf(stderr, "[RSX metal] depth/stencil buffer creation failed\n");
            return -1;
        }
        if (create_placeholders() != 0) {
            fprintf(stderr, "[RSX metal] placeholder texture creation failed\n");
            return -1;
        }

        s_verts = (MtlVertex*)malloc(sizeof(MtlVertex) * MTL_MAX_VERTS);
        if (!s_verts) {
            fprintf(stderr, "[RSX metal] vertex staging alloc failed\n");
            return -1;
        }

        NSError* serr = nil;
        s_shader_lib = [s_dev newLibraryWithSource:kBuiltinMSL options:nil error:&serr];
        if (!s_shader_lib) {
            fprintf(stderr, "[RSX metal] built-in shader compile failed: %s\n",
                    [[serr localizedDescription] UTF8String]);
            return -1;
        }

        /* PS3RECOMP_METAL_FIXED_FUNCTION=1 pins every draw to the built-in
         * shader: the first switch to flip when a title's draws vanish, to
         * tell a translation problem from a fetch or state one. */
        const char* ff = getenv("PS3RECOMP_METAL_FIXED_FUNCTION");
        s_guest_shaders = rsx_hlsl_to_msl_available() && !(ff && *ff && *ff != '0');

        s_inflight = dispatch_semaphore_create(MTL_MAX_INFLIGHT);
        if (!s_inflight) {
            fprintf(stderr, "[RSX metal] semaphore creation failed\n");
            return -1;
        }

        rsx_set_backend(&s_backend_vtable);
        s_ready  = 1;
        s_closed = 0;
        fprintf(stderr, "[RSX metal] %s on %s (%ux%u), guest shaders %s\n",
                s_headless ? "headless" : "windowed",
                [[s_dev name] UTF8String], s_width, s_height,
                s_guest_shaders ? "on" : (rsx_hlsl_to_msl_available() ? "off (env)" : "off (translator not built)"));
        return 0;
    }
}

void rsx_metal_backend_shutdown(void)
{
    @autoreleasepool {
        if (s_ready) rsx_set_backend(NULL);
        /* Reclaim every in-flight slot so no command buffer is still
         * referencing the device, queue or textures when they are released,
         * then hand the slots back. libdispatch traps if a semaphore is
         * deallocated while its count is below the value it was created with,
         * so draining without restoring is a crash, not a leak. */
        if (s_inflight) {
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_signal(s_inflight);
            s_inflight = nil;
        }
#if !TARGET_OS_IPHONE
        if (s_window) { [s_window close]; s_window = nil; }
#endif
        free(s_verts); s_verts = NULL;
        free(s_cb); s_cb = NULL; s_cb_used = s_cb_cap = 0;
        free(s_tex_staging); s_tex_staging = NULL; s_tex_staging_cap = 0;
        s_vert_count = s_rec_count = 0;
        s_guest_draws = s_last_guest_draws = 0;
        clear_caches();
        s_shader_lib = nil;
        s_null_tex   = nil;
        s_default_sampler = nil;
        s_ds_default = nil;
        s_layer     = nil;
        s_offscreen = nil;
        s_depth     = nil;
        s_queue     = nil;
        s_dev       = nil;
        s_ready     = 0;
    }
}

int rsx_metal_backend_pump_messages(void)
{
#if !TARGET_OS_IPHONE
    if (s_headless || !s_ready) return 0;
    @autoreleasepool {
        for (;;) {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
        }
        if (s_window && ![s_window isVisible]) s_closed = 1;
    }
#endif
    return s_closed ? -1 : 0;
}

void rsx_metal_backend_present(void)
{
    if (!s_ready) return;
    /* Block only when MTL_MAX_INFLIGHT frames are already queued. */
    if (!s_headless) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
    @autoreleasepool {
        id<MTLTexture> target = nil;
        id<CAMetalDrawable> drawable = nil;

        if (s_headless) {
            target = s_offscreen;
        } else {
            drawable = [s_layer nextDrawable];
            if (!drawable) {            /* compositor is busy; skip this frame */
                dispatch_semaphore_signal(s_inflight);
                return;
            }
            target = [drawable texture];
        }
        if (!target) {
            if (!s_headless) dispatch_semaphore_signal(s_inflight);
            return;
        }

        /* The frame's first pass clears colour to the last value the guest
         * asked for, which is what this backend has always done and what keeps
         * a frame with no clear in it from presenting an undefined drawable.
         * Depth and stencil start at the nv40 reset values, unless a clear
         * ahead of the first draw says otherwise -- those clears are folded
         * into this pass rather than each opening one of their own. */
        MtlClear first;
        first.flags   = MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL;
        first.color   = s_clear_argb;
        first.depth   = 1.0f;
        first.stencil = 0;
        u32 r = 0;
        for (; r < s_rec_count && s_records[r].kind == MTL_REC_CLEAR; r++) {
            const MtlClear* c = &s_records[r].u.clear;
            if (c->flags & MTL_CLEAR_DEPTH)   first.depth   = c->depth;
            if (c->flags & MTL_CLEAR_STENCIL) first.stencil = c->stencil;
        }

        /* Designated rather than memset: the buffers are ARC-managed. */
        MtlPassState ps = { .vb = nil, .cbuf = nil };
        if (s_vert_count > 0) {
            ps.vb = [s_dev newBufferWithBytes:s_verts
                                       length:sizeof(MtlVertex) * s_vert_count
                                      options:MTLResourceStorageModeShared];
            if (s_cb_used)
                ps.cbuf = [s_dev newBufferWithBytes:s_cb length:s_cb_used
                                            options:MTLResourceStorageModeShared];
        }

        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = begin_pass(cb, target, &first, &ps);
        for (; enc && r < s_rec_count; r++) {
            if (s_records[r].kind == MTL_REC_CLEAR) {
                [enc endEncoding];
                enc = begin_pass(cb, target, &s_records[r].u.clear, &ps);
                continue;
            }
            encode_draw(enc, &s_records[r].u.draw, &ps);
        }
        if (enc) [enc endEncoding];

        if (drawable) [cb presentDrawable:drawable];

        if (s_headless) {
            /* The readback below must see a completed frame. */
            [cb commit];
            [cb waitUntilCompleted];
        } else {
            dispatch_semaphore_t sem = s_inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _unused) {
                (void)_unused;
                dispatch_semaphore_signal(sem);
            }];
            [cb commit];        /* no wait: the CPU goes on to the next frame */
        }

        if (s_dropped_records) {
            fprintf(stderr, "[RSX metal] dropped %u record(s) this frame (cap %d records / %u verts)\n",
                    s_dropped_records, MTL_MAX_RECORDS, MTL_MAX_VERTS);
            s_dropped_records = 0;
        }
        s_rec_count  = 0;
        s_vert_count = 0;
        s_cb_used    = 0;
        s_last_guest_draws = s_guest_draws;
        s_guest_draws = 0;
        /* The frame's records are gone, so slots may move now. The command
         * buffer keeps its own references to whatever it still runs. */
        if (s_caches_full) {
            fprintf(stderr, "[RSX metal] cache full (%u VP, %u FP, %u PSO, %u textures); cleared\n",
                    s_vp_count, s_fp_count, s_pso_count, s_tex_count);
            clear_caches();
        }

        if (s_headless && s_offscreen) {
            u32 px = 0;
            MTLRegion r = MTLRegionMake2D(s_width / 2, s_height / 2, 1, 1);
            [s_offscreen getBytes:&px bytesPerRow:4 fromRegion:r mipmapLevel:0];
            s_last_present_bgra = px;
        }
    }
}

u32 rsx_metal_backend_debug_color(void)     { return s_clear_argb; }
u32 rsx_metal_backend_readback_center(void) { return s_last_present_bgra; }
u32 rsx_metal_backend_guest_draws(void)     { return s_last_guest_draws; }
