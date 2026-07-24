#!/usr/bin/env python3
"""Focused regression checks for PPU branch-with-link emission."""

from ppu_disasm import Instruction
from ppu_lifter import LiftedFunction, PPULifter


def check(label, lifter, func, mnemonic, operands, *expected):
    insn = Instruction(addr=0x30000, mnemonic=mnemonic, operands=operands)
    code = lifter._translate(insn, func)
    missing = [part for part in expected if part not in code]
    if missing:
        raise AssertionError(f"{label}: {code!r} missing {missing!r}")
    print(f"ok {label}: {code}")


def main():
    lifter = PPULifter()
    func = LiftedFunction(name="test", start_addr=0, end_addr=0x10000)

    check("bl", lifter, func, "bl", "0x1234",
          "ctx->lr = 0x00030004", "func_00001234(ctx)")
    check("bla", lifter, func, "bla", "0x1234",
          "ctx->lr = 0x00030004", "func_00001234(ctx)")
    check("bctrl", lifter, func, "bctrl", "",
          "ctx->lr = 0x00030004u", "ps3_indirect_call(ctx)")
    check("blrl", lifter, func, "blrl", "",
          "_target = (uint32_t)ctx->lr", "ctx->lr = 0x00030004u",
          "ctx->ctr = _target")
    check("beql", lifter, func, "beql", "0x1234",
          "ctx->lr = 0x00030004u", "func_00001234(ctx)")
    check("beqctrl", lifter, func, "beqctrl", "",
          "ctx->lr = 0x00030004u", "ps3_indirect_call(ctx)")
    check("beqlrl", lifter, func, "beqlrl", "",
          "_target = (uint32_t)ctx->lr", "ctx->lr = 0x00030004u",
          "ctx->ctr = _target")


if __name__ == "__main__":
    main()
