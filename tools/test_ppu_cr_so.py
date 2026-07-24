#!/usr/bin/env python3
"""Focused conformance checks for CR result fields that include XER[SO]."""

import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import ppu_disasm
from ppu_lifter import LiftedFunction, PPULifter, SOURCE_PREAMBLE


def cmp_form(xo, bf, l, ra, rb):
    return (31 << 26) | (bf << 23) | (l << 21) | (ra << 16) | (rb << 11) | (xo << 1)


def cmpi_form(op, bf, l, ra, imm):
    return (op << 26) | (bf << 23) | (l << 21) | (ra << 16) | (imm & 0xFFFF)


def lift(word, addr):
    insn = ppu_disasm.decode(word, addr)
    fn = LiftedFunction(name="cr_so", start_addr=addr, end_addr=addr + 4)
    return insn.mnemonic, PPULifter()._translate(insn, fn)


def main():
    for expected in (
        "static inline uint32_t ppu_res_cr0(int ok, uint32_t xer)",
        "uint32_t cr0 = ppu_res_cr0(ok, ctx->xer);",
    ):
        if expected not in SOURCE_PREAMBLE:
            raise RuntimeError(f"reservation helper is missing: {expected}")

    cases = [
        ("cmpwi eq SO=0", cmpi_form(11, 0, 0, 4, 5), 5, 0, 0x2, 28),
        ("cmpwi eq SO=1", cmpi_form(11, 0, 0, 4, 5), 5, 0, 0x3, 28),
        ("cmpdi lt SO=1", cmpi_form(11, 0, 1, 4, 5), 4, 0, 0x9, 28),
        ("cmplwi gt SO=1", cmpi_form(10, 2, 0, 4, 5), 9, 0, 0x5, 20),
        ("cmpw eq SO=1", cmp_form(0, 0, 0, 4, 5), 3, 3, 0x3, 28),
        ("cmpd lt SO=1", cmp_form(0, 0, 1, 4, 5), 1, 2, 0x9, 28),
        ("cmplw gt SO=1", cmp_form(32, 7, 0, 4, 5), 2, 1, 0x5, 0),
        ("cmpld lt SO=1", cmp_form(32, 7, 1, 4, 5), 1, 2, 0x9, 0),
    ]

    blocks = []
    addr = 0x10000
    for name, word, a, b, want, shift in cases:
        mnemonic, code = lift(word, addr)
        addr += 4
        if mnemonic != name.split()[0]:
            raise RuntimeError(f"{name}: decoded as {mnemonic}")
        so = 1 if "SO=1" in name else 0
        blocks.append(f"""
    memset(ctx, 0, sizeof(*ctx));
    ctx->gpr[4] = 0x{a:016X}ULL;
    ctx->gpr[5] = 0x{b:016X}ULL;
    ctx->xer = {so}u << 31;
    {code}
    check("{name}", (ctx->cr >> {shift}) & 0xFu, 0x{want:X}u);
""")

    source = f"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ppu_context.h"

static int failures;
static void check(const char* name, uint32_t got, uint32_t want) {{
    if (got != want) {{
        printf("FAIL %s: CR field %X, want %X\\n", name, got, want);
        failures++;
    }}
}}

int main() {{
    ppu_context storage;
    ppu_context* ctx = &storage;
    {''.join(blocks)}
    printf("[ppu-cr-so] 8 compare cases + reservation helper, %d failed\\n", failures);
    return failures ? 1 : 0;
}}
"""

    scratch = os.path.join(ROOT, "scratch")
    os.makedirs(scratch, exist_ok=True)
    cpath = os.path.join(scratch, "ppu_cr_so.cpp")
    epath = os.path.join(scratch, "ppu_cr_so.exe")
    with open(cpath, "w", newline="\n") as f:
        f.write(source)

    compiler = r"C:\Program Files\LLVM\bin\clang-cl.exe"
    result = subprocess.run([
        compiler, "/nologo", "/O1", "/W3",
        f"/I{os.path.join(ROOT, 'runtime', 'ppu')}",
        f"/Fe:{epath}", cpath,
    ], cwd=ROOT)
    if result.returncode:
        return result.returncode
    return subprocess.run([epath], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
