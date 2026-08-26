#!/usr/bin/env python3
"""Self-check: the lifter emits one definition per start address.

Two discovery passes can seed the same function start with different ends.
Emitting both is a C redefinition error (the file never compiles), and keeping
the shorter one silently drops real instructions -- libsre's 0x30001848 lost
its `stdu r1,-0xB0(r1)` frame setup and six register saves that way, which
would corrupt the stack of anything that called it.

Run: python tools/test_lifter_dedupe.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppu_lifter import LiftedFunction, PPULifter  # noqa: E402


def _lifter():
    try:
        return PPULifter.__new__(PPULifter)
    except Exception as exc:  # pragma: no cover
        print(f"cannot construct lifter: {exc}")
        raise


def main() -> int:
    lf = _lifter()
    lf.functions = [
        # the truncated copy comes first, exactly as the libsre lift emitted it
        LiftedFunction(name="f_1848", start_addr=0x1848, end_addr=0x1850,
                       body_lines=["/* truncated */"]),
        LiftedFunction(name="f_1848", start_addr=0x1848, end_addr=0x1874,
                       body_lines=["/* full */"]),
        LiftedFunction(name="f_1874", start_addr=0x1874, end_addr=0x1900,
                       body_lines=["/* other */"]),
    ]
    out = lf._emit_functions()

    starts = [f.start_addr for f in out]
    assert len(starts) == len(set(starts)), f"duplicate starts survived: {starts}"
    assert len(out) == 2, f"expected 2 functions, got {len(out)}"

    kept = next(f for f in out if f.start_addr == 0x1848)
    assert kept.end_addr == 0x1874, (
        f"kept the truncated body (end=0x{kept.end_addr:X}); widest extent must win")

    # order is preserved: 0x1848 still precedes 0x1874
    assert starts == [0x1848, 0x1874], f"emission order changed: {starts}"

    # a list with no duplicates must pass through untouched
    lf.functions = [
        LiftedFunction(name="a", start_addr=0x10, end_addr=0x20),
        LiftedFunction(name="b", start_addr=0x20, end_addr=0x30),
    ]
    assert len(lf._emit_functions()) == 2

    print("ok: truncated duplicate dropped, widest extent kept, order preserved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
