/*
 * ps3recomp - self-contained tests for RSX texture layout
 *
 * Build and run directly, no framework:
 *   cc -I include -I libs/video -o /tmp/t libs/video/tests/test_texture_layout.c \
 *      libs/video/rsx_texture_layout.c && /tmp/t
 *
 * The expectations are the behaviour rsx_d3d12_backend.c's uploader had before
 * the layout maths moved out of it, so this is a regression net for the
 * extraction as much as a unit test.
 */
#include "rsx_texture_layout.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

#define CHECK_EQ(got, want)                                                \
    do { unsigned long long _g = (unsigned long long)(got),                \
                            _w = (unsigned long long)(want);               \
         if (_g != _w) { printf("FAIL %s:%d  %s = %llu, want %llu\n",      \
                                __FILE__, __LINE__, #got, _g, _w);         \
                         g_fail++; } } while (0)

/* NV4097 texture format bytes. 0x20 is the LN (linear) flag. */
#define FMT_A8R8G8B8 0x85u
#define FMT_G8B8     0x8Bu
#define FMT_DXT1     0x86u
#define FMT_DXT23    0x87u
#define FMT_DXT45    0x88u
#define FMT_LN       0x20u

static void test_argb(void)
{
    rsx_tex_layout L;
    /* Swizzled UI art: no LN bit, power-of-two dims. */
    rsx_texture_layout(FMT_A8R8G8B8, 256, 256, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_R8G8B8A8);
    CHECK_EQ(L.bytes_per_texel, 4);
    CHECK(!L.compressed);
    CHECK(L.swizzled);
    CHECK_EQ(L.row_bytes, 256u * 4u);
    CHECK_EQ(L.rows, 256);
    CHECK_EQ(L.face_bytes, 256u * 256u * 4u);

    /* The LN bit turns swizzling off and changes nothing else. */
    rsx_texture_layout(FMT_A8R8G8B8 | FMT_LN, 256, 256, &L);
    CHECK(!L.swizzled);
    CHECK_EQ(L.fmt, RSX_TEXFMT_R8G8B8A8);
    CHECK_EQ(L.face_bytes, 256u * 256u * 4u);

    /* Non-power-of-two cannot be swizzled even with the LN bit clear -- the
     * hardware only swizzles POT, and treating NPOT as swizzled would read
     * the image apart. */
    rsx_texture_layout(FMT_A8R8G8B8, 100, 100, &L);
    CHECK(!L.swizzled);
    CHECK_EQ(L.row_bytes, 400);

    /* One POT axis is not enough. */
    rsx_texture_layout(FMT_A8R8G8B8, 256, 100, &L);
    CHECK(!L.swizzled);
}

static void test_g8b8(void)
{
    rsx_tex_layout L;
    /* The 1024x2048 font atlas: two bytes per texel, linear. */
    rsx_texture_layout(FMT_G8B8 | FMT_LN, 1024, 2048, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_R8G8);
    CHECK_EQ(L.bytes_per_texel, 2);
    CHECK(!L.compressed);
    CHECK(!L.swizzled);
    CHECK_EQ(L.row_bytes, 2048);
    CHECK_EQ(L.rows, 2048);
    CHECK_EQ(L.face_bytes, 1024u * 2048u * 2u);
}

static void test_compressed(void)
{
    rsx_tex_layout L;
    /* DXT1: 8-byte blocks, one block row per 4 texel rows. */
    rsx_texture_layout(FMT_DXT1, 512, 512, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_BC1);
    CHECK(L.compressed);
    CHECK_EQ(L.block_bytes, 8);
    CHECK_EQ(L.bytes_per_texel, 0);
    CHECK_EQ(L.row_bytes, (512u / 4u) * 8u);
    CHECK_EQ(L.rows, 512u / 4u);
    CHECK_EQ(L.face_bytes, (512u / 4u) * 8u * (512u / 4u));

    /* Compressed data is NEVER swizzled, LN bit clear or not. */
    CHECK(!L.swizzled);

    rsx_texture_layout(FMT_DXT23, 512, 512, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_BC2);
    CHECK_EQ(L.block_bytes, 16);
    CHECK_EQ(L.row_bytes, (512u / 4u) * 16u);

    rsx_texture_layout(FMT_DXT45, 64, 32, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_BC3);
    CHECK_EQ(L.block_bytes, 16);
    CHECK_EQ(L.rows, 8);

    /* Dimensions that are not a multiple of 4 round UP to whole blocks;
     * rounding down would truncate the last row and column. */
    rsx_texture_layout(FMT_DXT1, 10, 10, &L);
    CHECK_EQ(L.row_bytes, 3u * 8u);
    CHECK_EQ(L.rows, 3);
}

static void test_unknown_format_degrades(void)
{
    rsx_tex_layout L;
    /* Anything unrecognised falls back to one byte per texel rather than
     * guessing wider and reading past the end of guest memory. */
    rsx_texture_layout(0x81u, 64, 64, &L);
    CHECK_EQ(L.fmt, RSX_TEXFMT_R8);
    CHECK_EQ(L.bytes_per_texel, 1);
    CHECK(!L.compressed);
    CHECK_EQ(L.face_bytes, 64u * 64u);
}

static void test_swizzle_offset(void)
{
    /* 4x4 Morton order: x and y bits interleave, x first. */
    CHECK_EQ(rsx_swizzle_offset(0, 0, 2, 2), 0);
    CHECK_EQ(rsx_swizzle_offset(1, 0, 2, 2), 1);
    CHECK_EQ(rsx_swizzle_offset(0, 1, 2, 2), 2);
    CHECK_EQ(rsx_swizzle_offset(1, 1, 2, 2), 3);
    CHECK_EQ(rsx_swizzle_offset(2, 0, 2, 2), 4);
    CHECK_EQ(rsx_swizzle_offset(3, 3, 2, 2), 15);

    /* Every texel of a 4x4 image maps to a distinct offset in [0,16). */
    int seen[16] = {0};
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u32 o = rsx_swizzle_offset(x, y, 2, 2);
            CHECK(o < 16);
            if (o < 16) { CHECK_EQ(seen[o], 0); seen[o] = 1; }
        }

    /* Non-square: once the short axis runs out, the long axis's remaining
     * bits are appended above the interleaved part. */
    CHECK_EQ(rsx_swizzle_offset(5, 1, 3, 1), 11);

    /* 8x2 covers [0,16) exactly once, same as the square case. */
    int seen2[16] = {0};
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 8; x++) {
            u32 o = rsx_swizzle_offset(x, y, 3, 1);
            CHECK(o < 16);
            if (o < 16) { CHECK_EQ(seen2[o], 0); seen2[o] = 1; }
        }
}

static void test_log2_ceil(void)
{
    CHECK_EQ(rsx_log2_ceil(1), 0);
    CHECK_EQ(rsx_log2_ceil(2), 1);
    CHECK_EQ(rsx_log2_ceil(3), 2);   /* ceil, not floor */
    CHECK_EQ(rsx_log2_ceil(4), 2);
    CHECK_EQ(rsx_log2_ceil(256), 8);
    CHECK_EQ(rsx_log2_ceil(1024), 10);
}

/* --- decode ------------------------------------------------------------- */

/* A8R8G8B8 source is A,R,G,B per texel; the host wants R,G,B,A. */
static void test_decode_argb_linear(void)
{
    rsx_tex_layout L;
    rsx_texture_layout(FMT_A8R8G8B8 | FMT_LN, 2, 2, &L);
    CHECK(!L.swizzled);

    /* four texels, each A,R,G,B */
    const u8 src[16] = {
        0x11,0x22,0x33,0x44,   0x55,0x66,0x77,0x88,
        0x99,0xAA,0xBB,0xCC,   0xDD,0xEE,0xFF,0x01,
    };
    u8 dst[2 * 16];                    /* pitch deliberately > row_bytes */
    memset(dst, 0xA5, sizeof dst);
    rsx_texture_decode(dst, 16, src, 2, 2, &L, 0);

    /* row 0: R,G,B,A of each source texel */
    CHECK_EQ(dst[0], 0x22); CHECK_EQ(dst[1], 0x33);
    CHECK_EQ(dst[2], 0x44); CHECK_EQ(dst[3], 0x11);
    CHECK_EQ(dst[4], 0x66); CHECK_EQ(dst[5], 0x77);
    CHECK_EQ(dst[6], 0x88); CHECK_EQ(dst[7], 0x55);
    /* row 1 starts at the PITCH, not at row_bytes */
    CHECK_EQ(dst[16], 0xAA); CHECK_EQ(dst[19], 0x99);
    /* padding between row_bytes and pitch is left alone */
    CHECK_EQ(dst[8], 0xA5);

    /* TEX_RGBA: bytes are already R,G,B,A and pass straight through. */
    memset(dst, 0xA5, sizeof dst);
    rsx_texture_decode(dst, 16, src, 2, 2, &L, 1);
    CHECK_EQ(dst[0], 0x11); CHECK_EQ(dst[1], 0x22);
    CHECK_EQ(dst[2], 0x33); CHECK_EQ(dst[3], 0x44);
}

/* A swizzled image must come out in raster order. Build the source so that
 * texel (x,y) holds a known value AT ITS MORTON OFFSET, then check the decode
 * lays it back out row by row. */
static void test_decode_swizzled(void)
{
    rsx_tex_layout L;
    rsx_texture_layout(0x81u, 4, 4, &L);          /* one byte per texel */
    CHECK_EQ(L.bytes_per_texel, 1);
    CHECK(L.swizzled);                            /* no LN bit, POT dims */

    u8 src[16];
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            src[rsx_swizzle_offset(x, y, 2, 2)] = (u8)(y * 4 + x);

    u8 dst[4 * 8];
    memset(dst, 0xA5, sizeof dst);
    rsx_texture_decode(dst, 8, src, 4, 4, &L, 0);

    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            CHECK_EQ(dst[y * 8 + x], y * 4 + x);

    /* Same bytes read as LINEAR must NOT come out in raster order -- otherwise
     * the swizzled path is not doing anything and the test proves nothing. */
    rsx_tex_layout Lin;
    rsx_texture_layout(0x81u | FMT_LN, 4, 4, &Lin);
    CHECK(!Lin.swizzled);
    u8 dst2[4 * 8];
    rsx_texture_decode(dst2, 8, src, 4, 4, &Lin, 0);
    int differs = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            if (dst2[y * 8 + x] != dst[y * 8 + x]) differs = 1;
    CHECK(differs);
}

/* Two-byte texels deswizzle as a unit: both bytes move together. */
static void test_decode_g8b8_swizzled(void)
{
    rsx_tex_layout L;
    rsx_texture_layout(FMT_G8B8, 4, 4, &L);
    CHECK_EQ(L.bytes_per_texel, 2);
    CHECK(L.swizzled);

    u8 src[32];
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u32 o = rsx_swizzle_offset(x, y, 2, 2) * 2;
            src[o + 0] = (u8)(0x10 + y * 4 + x);
            src[o + 1] = (u8)(0x80 + y * 4 + x);
        }

    u8 dst[4 * 16];
    rsx_texture_decode(dst, 16, src, 4, 4, &L, 0);
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            CHECK_EQ(dst[y * 16 + x * 2 + 0], 0x10 + y * 4 + x);
            CHECK_EQ(dst[y * 16 + x * 2 + 1], 0x80 + y * 4 + x);
        }
}

/* Compressed payload is copied verbatim -- BC1/2/3 are bit-identical to
 * DXT1/23/45 -- with only the row stride changing. */
static void test_decode_compressed_is_verbatim(void)
{
    rsx_tex_layout L;
    rsx_texture_layout(FMT_DXT1, 8, 8, &L);
    CHECK_EQ(L.row_bytes, 16);        /* 2 blocks of 8 bytes */
    CHECK_EQ(L.rows, 2);

    u8 src[32];
    for (u32 i = 0; i < sizeof src; i++) src[i] = (u8)(i * 7 + 1);

    u8 dst[2 * 24];
    memset(dst, 0xA5, sizeof dst);
    rsx_texture_decode(dst, 24, src, 8, 8, &L, 0);

    for (u32 y = 0; y < 2; y++)
        for (u32 b = 0; b < 16; b++)
            CHECK_EQ(dst[y * 24 + b], src[y * 16 + b]);
    CHECK_EQ(dst[16], 0xA5);          /* padding untouched */
}

/* Nothing should be written through a null or zero-sized request. */
static void test_decode_rejects_nonsense(void)
{
    rsx_tex_layout L;
    rsx_texture_layout(FMT_A8R8G8B8 | FMT_LN, 2, 2, &L);
    u8 dst[16];
    memset(dst, 0xA5, sizeof dst);
    const u8 src[16] = {0};
    rsx_texture_decode(NULL, 8, src, 2, 2, &L, 0);
    rsx_texture_decode(dst, 8, NULL, 2, 2, &L, 0);
    rsx_texture_decode(dst, 8, src, 0, 2, &L, 0);
    rsx_texture_decode(dst, 8, src, 2, 2, NULL, 0);
    for (u32 i = 0; i < sizeof dst; i++) CHECK_EQ(dst[i], 0xA5);
}

int main(void)
{
    test_argb();
    test_g8b8();
    test_compressed();
    test_unknown_format_degrades();
    test_swizzle_offset();
    test_log2_ceil();
    test_decode_argb_linear();
    test_decode_swizzled();
    test_decode_g8b8_swizzled();
    test_decode_compressed_is_verbatim();
    test_decode_rejects_nonsense();

    if (g_fail) { printf("\nRSX texture layout: %d FAILED\n", g_fail); return 1; }
    printf("RSX texture layout tests: all passed\n");
    return 0;
}
