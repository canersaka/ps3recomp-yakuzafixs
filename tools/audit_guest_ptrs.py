#!/usr/bin/env python3
"""Find HLE functions that dereference a pointer parameter as a HOST pointer.

The HLE ABI adapter (ppu_hle.cpp) passes a guest function's arguments straight
through as the raw PPC register values. A pointer parameter is therefore a
GUEST address. Dereferencing it directly writes to (or reads from) whatever
host address happens to share that number -- usually an access violation, and
occasionally silent corruption.

The convention that IS correct, spelled out in libs/video/cellGcmSys.c
(cellGcmGetConfiguration): translate through vm_base, or use vm_read*/vm_write*.

This scans for the mistake so a whole library can be fixed in one pass instead
of one crash at a time. It is a heuristic -- it reports candidates, it does not
edit anything.

    python tools/audit_guest_ptrs.py libs/            # everything
    python tools/audit_guest_ptrs.py libs/font        # one library

Output is one line per suspect function:

    libs/font/cellFont.c:412  cellFontOpenFontset  derefs: fontType, font
"""
import os
import re
import sys

# A function definition at column 0 returning a typical HLE type.
FUNC_RE = re.compile(
    r'^(?:s32|u32|s64|u64|int|void|float)\s+'      # return type
    r'([A-Za-z_][A-Za-z0-9_]*)\s*'                 # name
    r'\(([^)]*)\)\s*$',                            # params (single line)
    re.M)

# A pointer parameter: "const char* path", "CellFontType* fontType", "u32 *out"
PARAM_PTR_RE = re.compile(r'(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*([A-Za-z_][A-Za-z0-9_]*)\s*$')

# Uses that are already correct: the parameter is converted, not dereferenced.
SAFE_USE = re.compile(r'(?:uintptr_t\)\s*|vm_read\w*\(|vm_write\w*\(|gptr\(|gpath\(|guest_str\(|g_ps3_guest_caller\()')


def pointer_params(param_text):
    out = []
    for raw in param_text.split(','):
        raw = raw.strip()
        if not raw or raw == 'void':
            continue
        m = PARAM_PTR_RE.match(raw)
        if m:
            out.append(m.group(1))
    return out


def function_bodies(src):
    """Yield (name, params, start_line, body) for each column-0 definition."""
    for m in FUNC_RE.finditer(src):
        brace = src.find('{', m.end())
        if brace < 0:
            continue
        depth = 0
        i = brace
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield (m.group(1), m.group(2), src.count('\n', 0, m.start()) + 1,
               src[brace:i])


# Host functions that DEREFERENCE a pointer argument. Passing a guest pointer
# bare to any of these is the same bug as *p, and is how cellFsRead shipped an
# entire sound bank to an untranslated address -- fread(buf,...) reads no
# differently from *buf, but the old checks only looked for *p and p->.
DEREFERENCING = (
    'strcpy strncpy strcat strncat strlen strcmp strncmp strchr strrchr strstr'
    ' strdup sprintf snprintf sscanf atoi atol strtol strtoul strtod'
    ' memcpy memmove memset memcmp memchr'
    ' fopen freopen fread fwrite fputs fgets remove rename mkdir stat'
).split()

def bare_deref_calls(body, param):
    """Lines calling a dereferencing host function with `param` passed bare."""
    out = []
    for fn in DEREFERENCING:
        pat = re.escape(fn) + r"\s*\([^;]*?(?<![\w.>])" + re.escape(param) + r"\s*[,)]"
        for m in re.finditer(pat, body):
            nlpos = body.rfind(chr(10), 0, m.start()) + 1
            endpos = body.find(chr(10), m.start())
            line = body[nlpos:] if endpos < 0 else body[nlpos:endpos]
            if not SAFE_USE.search(line):
                out.append(line)
    return out

def suspects_in(path):
    src = open(path, 'rb').read().decode('utf-8', 'replace').replace('\r\n', '\n')
    found = []
    for name, params, line, body in function_bodies(src):
        ptrs = pointer_params(params)
        if not ptrs:
            continue
        bad = []
        for p in ptrs:
            # p-> or *p, but not when the line also converts it safely
            uses = re.findall(r'^.*(?:\*\s*%s\b|\b%s\s*->).*$' % (p, p), body, re.M)
            uses = [u for u in uses if not SAFE_USE.search(u)]
            # A declaration like "CellFsStat* sb = (...)" inside the body is not
            # a use. Require a TYPE NAME before the star: the old pattern also
            # matched a bare "*out = (u32)x", which is a real deref through a
            # parameter and the single most common form of this bug. cellSail's
            # "*handle = (u32)i" was invisible because of it, and so was every
            # other out-param write of that shape -- the audit reported 0
            # suspects for a file full of them.
            uses = [u for u in uses
                    if not re.search(r'[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*%s\s*=' % p, u)]
            uses += bare_deref_calls(body, p)
            if uses:
                bad.append(p)
        if bad:
            found.append((line, name, bad))
    return found


def main():
    roots = sys.argv[1:] or ['libs']
    total = 0
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for fn in sorted(files):
                if not fn.endswith('.c'):
                    continue
                path = os.path.join(dirpath, fn).replace('\\', '/')
                for line, name, bad in suspects_in(path):
                    print('%s:%d  %s  derefs: %s' % (path, line, name, ', '.join(bad)))
                    total += 1
    print('\n%d suspect function(s)' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
