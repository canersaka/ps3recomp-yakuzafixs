/*
 * ps3recomp - RSX texture layout: format class, sizes, and swizzle addressing
 *
 * Working out what shape a guest texture is -- how many bytes a row takes, how
 * many rows there are, whether the texels are Morton-ordered, where texel
 * (x, y) actually lives -- is RSX semantics. It has nothing to do with the host
 * graphics API, which only needs the answers.
 *
 * It lived inside rsx_d3d12_backend.c's uploader, which is why the Metal
 * backend has no textures at all: a second backend cannot sample a guest
 * texture without first re-deriving all of this. Pulling it out is what makes
 * that possible without a third copy (see rsx_vertex_fetch.h for how the
 * second copy of the vertex fetch worked out).
 *
 * Scope is the formats the D3D12 backend actually handles today. Everything
 * else falls back to one byte per texel, exactly as it did before, so an
 * unrecognised format degrades rather than reading out of bounds.
 */
#ifndef PS3RECOMP_RSX_TEXTURE_LAYOUT_H
#define PS3RECOMP_RSX_TEXTURE_LAYOUT_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-neutral format class. A backend maps these to its own enum -- DXGI,
 * MTLPixelFormat, VkFormat -- rather than each deriving them from the RSX
 * format byte. */
typedef enum {
    RSX_TEXFMT_R8 = 0,   /* B8, and the fallback for anything unrecognised */
    RSX_TEXFMT_R8G8,     /* G8B8                                          */
    RSX_TEXFMT_R8G8B8A8, /* A8R8G8B8                                      */
    RSX_TEXFMT_BC1,      /* DXT1                                          */
    RSX_TEXFMT_BC2,      /* DXT23                                         */
    RSX_TEXFMT_BC3       /* DXT45                                         */
} rsx_texfmt;

typedef struct {
    rsx_texfmt fmt;
    int  compressed;      /* block-compressed: rows are rows of 4x4 blocks   */
    int  swizzled;        /* texels are Morton/Z-ordered, not row-major      */
    u32  bytes_per_texel; /* 0 when compressed                               */
    u32  block_bytes;     /* 0 when not compressed (8 for BC1, else 16)      */
    u32  row_bytes;       /* one texel row, or one block row                 */
    u32  rows;            /* texel rows, or block rows                       */
    u32  face_bytes;      /* row_bytes * rows: one face, one mip level       */
} rsx_tex_layout;

/* Classify `rsx_fmt` (the NV4097 SET_TEXTURE_FORMAT byte) at `w` x `h`.
 *
 * `swizzled` is set when the LN bit (0x20) is clear, the format is not
 * compressed, and both dimensions are powers of two -- the hardware only
 * swizzles under those conditions, and compressed data is never swizzled.
 * Treating a swizzled texture as linear is what produced diagonal-stripe
 * garbage on LBP's loading screen, so the flag is load-bearing. */
void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out);

/* Byte offset of texel (x, y) within a swizzled (Morton-ordered) image whose
 * dimensions are 2^log2w by 2^log2h, in texels -- multiply by bytes_per_texel.
 * Interleaves the low bits of x and y, then appends whichever axis still has
 * bits left when the other is exhausted (non-square images). */
u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h);

/* ceil(log2(v)); 0 for v <= 1. */
u32 rsx_log2_ceil(u32 v);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_TEXTURE_LAYOUT_H */
