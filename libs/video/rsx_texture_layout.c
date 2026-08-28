/*
 * ps3recomp - RSX texture layout (see rsx_texture_layout.h)
 */
#include "rsx_texture_layout.h"

u32 rsx_log2_ceil(u32 v)
{
    u32 l = 0;
    while ((1u << l) < v) l++;
    return l;
}

u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; shift++;
        off |= (y & 1u) << shift; y >>= 1; shift++;
        log2w--; log2h--;
    }
    off |= (x | y) << shift;     /* only one of x/y still has bits */
    return off;
}

void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out)
{
    if (!out) return;

    /* Format classes (base = fmt & 0x9F, masking off the LN/UN flag bits).
     * The LBP loading screen exercises all of them: 0x85 A8R8G8B8 (swizzled UI
     * art), 0x8B G8B8 (the 1024x2048 linear font atlas -- without it no text
     * renders at all), 0x86/87/88 DXT1/23/45 (512x512 detail and LUT layers
     * bound on every draw). */
    u32 basef = rsx_fmt & 0x9Fu;

    out->compressed     = 0;
    out->block_bytes    = 0;
    out->bytes_per_texel = 1;
    out->fmt            = RSX_TEXFMT_R8;

    switch (basef) {
    case 0x85: out->fmt = RSX_TEXFMT_R8G8B8A8; out->bytes_per_texel = 4; break;
    case 0x8B: out->fmt = RSX_TEXFMT_R8G8;     out->bytes_per_texel = 2; break;
    case 0x86: out->fmt = RSX_TEXFMT_BC1; out->compressed = 1; out->block_bytes = 8;  break;
    case 0x87: out->fmt = RSX_TEXFMT_BC2; out->compressed = 1; out->block_bytes = 16; break;
    case 0x88: out->fmt = RSX_TEXFMT_BC3; out->compressed = 1; out->block_bytes = 16; break;
    default:   break;   /* R8, one byte per texel -- the pre-existing fallback */
    }

    if (out->compressed) {
        /* Compressed data is stored as linear rows of 4x4 blocks, and is never
         * Morton-swizzled on RSX. */
        out->bytes_per_texel = 0;
        out->swizzled = 0;
        out->row_bytes = ((w + 3u) / 4u) * out->block_bytes;
        out->rows      = (h + 3u) / 4u;
    } else {
        /* Swizzled unless the LN bit is set. The hardware requires power-of-two
         * dimensions to swizzle, so a NPOT image is linear regardless. */
        out->swizzled  = !(rsx_fmt & 0x20u) &&
                         w && h &&
                         (w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u;
        out->row_bytes = w * out->bytes_per_texel;
        out->rows      = h;
    }

    out->face_bytes = out->row_bytes * out->rows;
}
