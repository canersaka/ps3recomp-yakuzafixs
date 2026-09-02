/*
 * ps3recomp - guest shader -> MSL translation test
 *
 * Hand-assembles the two smallest programs the Metal backend has to run,
 * pushes each through its decompiler, then through rsx_hlsl_to_msl, and
 * checks the MSL carries the bindings the backend depends on. Needs no GPU,
 * so it runs on Linux CI too -- which is the point: glslang's HLSL front end
 * and spirv-cross are checked against the decompilers' actual output on every
 * push, not only on the Mac that happens to have Metal.
 *
 * Programs:
 *   VP  MOV o0, v0 ; MOV o1, v3 (END)      position through, colour through
 *   FP  MOV r0, COL0 (END)                 colour out
 *   FP  TEX r0, TC0 unit 0 (END)           texture unit 0 sampled at texcoord0
 *
 * The VP encoding is written out field by field below. It is the same layout
 * the decompiler documents in its header, and the test first checks the HLSL
 * says what was meant before translating it, so a wrong bit shows up as a
 * decompiler-level failure rather than a mysterious MSL one.
 *
 * Built as a CMake target (test_shader_msl) because it links glslang and
 * spirv-cross. -v prints the HLSL and MSL of every program.
 */
#include "../rsx_vp_decompiler.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_shader_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0, g_verbose = 0;

static void check(const char* what, const char* text, const char* needle)
{
    if (text && strstr(text, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", what, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\n", what, needle);
        if (!g_verbose && text) printf("----\n%s\n----\n", text);
        g_fail++;
    }
}

/* ---- NV40 vertex program encoding ---------------------------------------- */

static void put_le(u8* p, u32 w)
{
    p[0] = (u8)w; p[1] = (u8)(w >> 8); p[2] = (u8)(w >> 16); p[3] = (u8)(w >> 24);
}

/* A 17-bit source field: reg_type [0:1], reg [2:7], swizzle w/z/y/x [8:15]
 * two bits each (so identity is x=0 at [14:15], y=1 at [12:13], z=2 at
 * [10:11], w=3 at [8:9]), neg [16]. */
#define VP_SRC_INPUT  2u
#define VP_SRC_TEMP   1u
#define VP_SWZ_IDENT  ((3u << 8) | (2u << 10) | (1u << 12) | (0u << 14))
static u32 vp_src(u32 type, u32 reg) { return type | ((reg & 0x3Fu) << 2) | VP_SWZ_IDENT; }

/* One instruction: vector op `vec_op` reading input register `input` as src0,
 * writing all four lanes of output register `o`, no temp write, scalar NOP. */
static void vp_instr(u8* p, u32 vec_op, u32 input, u32 o, int end)
{
    const u32 src0 = vp_src(VP_SRC_INPUT, 0);   /* v[input_src], index from D1 */
    const u32 src1 = vp_src(VP_SRC_TEMP, 0);
    const u32 src2 = vp_src(VP_SRC_TEMP, 0);
    u32 d0 = (0x3Fu << 15)            /* dst_tmp: none            */
           | (1u << 30);              /* vec_result: write o[dst] */
    u32 d1 = (src0 >> 9)              /* src0 high 8 bits         */
           | ((input & 0xFu) << 8)    /* input_src                */
           | ((vec_op & 0x1Fu) << 22) /* vec_opcode               */
           | (0u << 27);              /* sca_opcode: NOP          */
    u32 d2 = (src2 >> 11)             /* src2 high 6 bits         */
           | ((src1 & 0x1FFFFu) << 6) /* src1                     */
           | ((src0 & 0x1FFu) << 23); /* src0 low 9 bits          */
    u32 d3 = (end ? 1u : 0u)          /* end                      */
           | ((o & 0x1Fu) << 2)       /* dst (output register)    */
           | (0x3Fu << 7)             /* sca_dst_tmp: none        */
           | (0xFu << 13)             /* vec_writemask xyzw       */
           | (0u << 17)               /* sca_writemask: none      */
           | ((src2 & 0x7FFu) << 21); /* src2 low 11 bits         */
    put_le(p + 0, d0); put_le(p + 4, d1); put_le(p + 8, d2); put_le(p + 12, d3);
}

/* ---- NV40 fragment program encoding -------------------------------------- */

/* rsx_fp_read_word does a big-endian load then a 16-bit half swap; store the
 * inverse so the decoder sees host word `h`. */
static void put_fp_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}
#define FP_OPC(x)       ((u32)(x) << 24)
#define FP_MASK_XYZW    (0xFu << 9)
#define FP_END          1u
#define FP_INSRC(x)     ((u32)(x) << 13)
#define FP_TEXU(x)      ((u32)(x) << 17)
#define FP_SWZ_IDENT    ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define FP_T_INPUT      1u
/* The execution condition lives in the SRC0 word: exec_if lt/eq/gt at bits
 * 18..20 (all three set = unconditional, none = never, which is what an
 * all-zero word means), cond swizzle x/y/z/w at 21..28. */
#define FP_EXEC_ALWAYS  ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))

static void fp_instr(u8* p, u32 w0, u32 w1)
{
    put_fp_word(p + 0, w0); put_fp_word(p + 4, w1);
    put_fp_word(p + 8, 0);  put_fp_word(p + 12, 0);
}

/* ---- the translation step, shared by every case -------------------------- */

static char g_hlsl[65536];
static char g_msl[65536];
static char g_log[4096];

static int translate(const char* what, int stage)
{
    if (g_verbose) printf("---- %s HLSL ----\n%s\n", what, g_hlsl);
    int rc = rsx_hlsl_to_msl(g_hlsl, stage, g_msl, sizeof g_msl, g_log, sizeof g_log);
    if (rc != 0) {
        printf("[FAIL] %s -- translation failed: %s\n", what, g_log);
        if (!g_verbose) printf("---- HLSL ----\n%s\n", g_hlsl);
        g_fail++;
        return -1;
    }
    printf("[PASS] %s -- translated (%u bytes of MSL)\n", what, (unsigned)strlen(g_msl));
    g_pass++;
    if (g_verbose) printf("---- %s MSL ----\n%s\n", what, g_msl);
    return 0;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;

    if (!rsx_hlsl_to_msl_available()) {
        printf("HLSL to MSL translation not built; nothing to test\n");
        return 2;
    }

    /* ---- vertex program: MOV o0, v0 ; MOV o1, v3 ------------------------- */
    {
        u8 vp[32];
        vp_instr(vp +  0, 0x01, 0, 0, 0);   /* MOV o0, v0 */
        vp_instr(vp + 16, 0x01, 3, 1, 1);   /* MOV o1, v3, END */

        int n = rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl);
        if (n != 2) { printf("[FAIL] VP decompile returned %d, expected 2\n", n); g_fail++; }
        else        { printf("[PASS] VP decompile: 2 instructions\n"); g_pass++; }
        /* The HLSL must say o0 takes v0 and o1 takes v3, unswizzled, before
         * the MSL is looked at. */
        check("VP HLSL", g_hlsl, "(v[0]).xyzw");
        check("VP HLSL", g_hlsl, "o[0].xyzw = _v.xyzw;");
        check("VP HLSL", g_hlsl, "(v[3]).xyzw");
        check("VP HLSL", g_hlsl, "o[1].xyzw = _v.xyzw;");

        if (translate("VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("VP MSL", g_msl, "vertex ");
            check("VP MSL", g_msl, "main0");
            /* The legalization pass drops inputs the program never reads,
             * so only v0 and v3 survive -- and v3 must still be attribute 3,
             * not renumbered to 1: the backend's vertex descriptor puts RSX
             * attribute i at offset i*16, used or not. */
            check("VP MSL", g_msl, "[[attribute(0)]]");
            check("VP MSL", g_msl, "[[attribute(3)]]");
            check("VP MSL", g_msl, "[[buffer(0)]]");      /* VPConst */
            check("VP MSL", g_msl, "[[position]]");
            check("VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
    }

    /* ---- fragment program: MOV r0, COL0 ---------------------------------- */
    {
        u8 fp[16];
        fp_instr(fp, FP_OPC(0x01) | FP_MASK_XYZW | FP_INSRC(1) | FP_END,
                     FP_T_INPUT | FP_SWZ_IDENT | FP_EXEC_ALWAYS);
        u32 nconst = 0;
        int n = rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u /* r0 exports */, 0,
                                             g_hlsl, sizeof g_hlsl, &nconst);
        if (n != 1) { printf("[FAIL] FP decompile returned %d, expected 1\n", n); g_fail++; }
        else        { printf("[PASS] FP decompile: 1 instruction\n"); g_pass++; }
        check("FP HLSL", g_hlsl, "input.col0");
        check("FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        check("FP HLSL", g_hlsl, "cbuffer PSConstants : register(b1)");
        /* The alpha test patch must survive translation as well. */
        if (rsx_fp_apply_alpha_test_buffered(g_hlsl, sizeof g_hlsl, 0x204u /* GREATER */) < 0) {
            printf("[FAIL] FP alpha test patch\n"); g_fail++;
        } else { printf("[PASS] FP alpha test patch\n"); g_pass++; }
        check("FP HLSL", g_hlsl, "fp_alpha.x");

        if (translate("FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("FP MSL", g_msl, "fragment ");
            check("FP MSL", g_msl, "main0");
            check("FP MSL", g_msl, "[[buffer(1)]]");      /* PSConstants */
            check("FP MSL", g_msl, "[[color(0)]]");
            check("FP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 from the VP */
            check("FP MSL", g_msl, "discard_fragment");
        }
    }

    /* ---- fragment program: TEX r0, TC0 (unit 0) --------------------------- */
    {
        u8 fp[16];
        fp_instr(fp, FP_OPC(0x17) | FP_MASK_XYZW | FP_INSRC(4) | FP_TEXU(0) | FP_END,
                     FP_T_INPUT | FP_SWZ_IDENT | FP_EXEC_ALWAYS);
        int n = rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                             g_hlsl, sizeof g_hlsl, NULL);
        if (n != 1) { printf("[FAIL] TEX FP decompile returned %d, expected 1\n", n); g_fail++; }
        else        { printf("[PASS] TEX FP decompile: 1 instruction\n"); g_pass++; }
        check("TEX FP HLSL", g_hlsl, "rsx_tex[0].Sample(rsx_samp[0]");
        check("TEX FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");

        if (translate("TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("TEX FP MSL", g_msl, "[[texture(0)]]");
            check("TEX FP MSL", g_msl, "[[sampler(0)]]");
            check("TEX FP MSL", g_msl, "[[user(locn3)]]");    /* TEXCOORD0 */
        }
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
