/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * The first non-Windows render path in the project. It implements the subset of
 * the rsx_backend vtable needed to drive a guest clear/flip loop: `clear`
 * captures the colour written by NV4097_CLEAR_SURFACE, `set_render_target`
 * tracks the guest's surface dimensions, and present paints a drawable with it.
 * The remaining callbacks stay NULL -- every dispatch site in rsx_commands.c is
 * guarded (`if (s_backend && s_backend->x)`), so a partial vtable is the
 * intended way to bring a backend up incrementally.
 *
 * Why Metal rather than SDL_Renderer: SDL2's renderer is a 2D sprite API with
 * no route to a custom vertex program, depth/stencil, MRT or render-to-texture,
 * so it cannot grow into the real pipeline. The shader path this backend will
 * need is already proven end to end -- the existing rsx_fp/vp decompilers emit
 * HLSL, glslang lowers it to SPIR-V, spirv-cross emits MSL, and
 * -newLibraryWithSource: compiles it at runtime with no full Xcode install.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#if !TARGET_OS_IPHONE
#  import <AppKit/AppKit.h>
#endif

#include "rsx_commands.h"
#include "rsx_metal_backend.h"
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */

static id<MTLDevice>       s_dev;
static id<MTLCommandQueue> s_queue;
static CAMetalLayer*       s_layer;      /* windowed only   */
static id<MTLTexture>      s_offscreen;  /* headless only   */
static NSWindow*           s_window;
static int                 s_headless;
static int                 s_closed;
static int                 s_ready;
static u32                 s_width  = 1280;
static u32                 s_height = 720;

/* RSX clear colour, ARGB8888, as written by NV4097_SET_COLOR_CLEAR_VALUE. */
static u32 s_clear_argb = 0xFF000000u;
static u32 s_last_present_bgra;

/* ---- rsx_backend vtable -------------------------------------------------- */

static void mtl_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)depth; (void)stencil;
    /* 0xF0 is the colour-buffer mask; depth/stencil clears carry 0x03. */
    if (flags & 0xF0u) s_clear_argb = color;
}

static void mtl_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    if (!state) return;
    u32 w = state->surface_clip_w, h = state->surface_clip_h;
    if (!w || !h || (w == s_width && h == s_height)) return;
    s_width = w; s_height = h;
    if (s_layer) s_layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
}

static rsx_backend s_backend_vtable = {
    .userdata          = NULL,
    .clear             = mtl_clear,
    .set_render_target = mtl_set_render_target,
};

/* ---- helpers ------------------------------------------------------------- */

static MTLClearColor clear_color_from_rsx(void)
{
    /* ARGB8888 -> normalised RGBA. sRGB conversion is deliberately skipped:
     * the D3D12 backend treats the guest value as raw UNORM too, so both
     * backends agree pixel-for-pixel. */
    const double a = (double)((s_clear_argb >> 24) & 0xFF) / 255.0;
    const double r = (double)((s_clear_argb >> 16) & 0xFF) / 255.0;
    const double g = (double)((s_clear_argb >>  8) & 0xFF) / 255.0;
    const double b = (double)( s_clear_argb        & 0xFF) / 255.0;
    return MTLClearColorMake(r, g, b, a);
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

        rsx_set_backend(&s_backend_vtable);
        s_ready  = 1;
        s_closed = 0;
        fprintf(stderr, "[RSX metal] %s on %s (%ux%u)\n",
                s_headless ? "headless" : "windowed",
                [[s_dev name] UTF8String], s_width, s_height);
        return 0;
    }
}

void rsx_metal_backend_shutdown(void)
{
    @autoreleasepool {
        if (s_ready) rsx_set_backend(NULL);
#if !TARGET_OS_IPHONE
        if (s_window) { [s_window close]; s_window = nil; }
#endif
        s_layer     = nil;
        s_offscreen = nil;
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
    @autoreleasepool {
        id<MTLTexture> target = nil;
        id<CAMetalDrawable> drawable = nil;

        if (s_headless) {
            target = s_offscreen;
        } else {
            drawable = [s_layer nextDrawable];
            if (!drawable) return;      /* compositor is busy; skip this frame */
            target = [drawable texture];
        }
        if (!target) return;

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = target;
        rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor  = clear_color_from_rsx();
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        /* Draw calls land here once the PSO/shader path is wired; the clear
         * alone is what the guest flip loop needs today. */
        [enc endEncoding];

        if (drawable) [cb presentDrawable:drawable];
        [cb commit];
        [cb waitUntilCompleted];

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
