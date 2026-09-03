# Game Porting Guide

A comprehensive, step-by-step guide to porting a PS3 game to native PC using ps3recomp.

---

## Table of Contents

1. [Overview](#overview)
2. [Phase 1: Game Selection and Assessment](#phase-1-game-selection-and-assessment)
3. [Phase 2: Binary Acquisition and Decryption](#phase-2-binary-acquisition-and-decryption)
4. [Phase 3: Analysis](#phase-3-analysis)
5. [Phase 4: Disassembly](#phase-4-disassembly)
6. [Phase 5: Code Generation](#phase-5-code-generation)
7. [Phase 6: Project Setup](#phase-6-project-setup)
8. [Phase 7: Building and Debugging](#phase-7-building-and-debugging)
9. [Phase 8: Stubbing and Implementation](#phase-8-stubbing-and-implementation)
10. [Phase 9: Graphics](#phase-9-graphics)
11. [Phase 10: Audio and Input](#phase-10-audio-and-input)
12. [Phase 11: SPU Handling](#phase-11-spu-handling)
13. [Phase 12: Testing and Polish](#phase-12-testing-and-polish)
14. [Platform Notes](#platform-notes)
15. [Tips and Best Practices](#tips-and-best-practices)
16. [Case Study: flOw](#case-study-flow)

---

## Overview

Porting a PS3 game with ps3recomp is a **per-game project**. Unlike emulation (where one binary runs many games), static recompilation requires understanding each game's specific needs. The reward is a truly native port with full modding potential.

### What You'll Need

- A legally obtained copy of the PS3 game
- Decryption keys or a fSELF version of the game binary
- Python 3.9+ for the recompiler tools
- CMake 3.20+ and a C17/C++20 compiler
- RPCS3 as a reference (for testing and behavior verification)
- Patience and debugging skills

### Expected Timeline

| Game Complexity | Estimate | Examples |
|---|---|---|
| Simple PSN title | Days to weeks | flOw, fl0w, Flower |
| Standard indie game | Weeks to months | Journey, Limbo, Braid |
| Full retail title | Months | Standard action/adventure games |
| Complex AAA title | Months to years | The Last of Us, Uncharted |

The main variables are: how many unique system APIs the game uses, whether it uses SPU programs extensively, and how complex its graphics pipeline is.

---

## Phase 1: Game Selection and Assessment

### Choose a Good First Target

For your first port, look for games that:

1. **Work well in RPCS3** — this means the game's behavior is well-understood
2. **Are simple** — fewer system API calls = fewer HLE modules needed
3. **Use common engines** — PhyreEngine, Unreal Engine 3, etc. have known patterns
4. **Don't rely on SPU-heavy compute** — audio-only SPU use is much simpler than physics/particle SPU use
5. **Have a small binary** — fewer functions to recompile and debug

### Pre-Analysis with RPCS3

Before touching ps3recomp, run the game in RPCS3 with logging enabled:

1. Enable `Log Level: All` in RPCS3 settings
2. Play through the game's startup and early gameplay
3. Check the log for:
   - Which modules are loaded (`cellSysmoduleLoadModule`)
   - Which NID calls are frequent
   - Any "TODO" or "Unimplemented" warnings
   - SPU thread creation patterns

This tells you which HLE modules the game needs most.

### Check Module Coverage

Cross-reference the game's imports against [MODULE_STATUS.md](MODULE_STATUS.md):

```bash
# After ELF analysis
python tools/prx_analyzer.py game/EBOOT.ELF --stubs
```

If coverage is 95%+, the game is a good candidate.

---

## Phase 2: Binary Acquisition and Decryption

### Getting the ELF

PS3 game binaries are encrypted in SELF (Signed ELF) format. You need a decrypted ELF:

**Option A: fSELF (fake SELF)**
- Some development/debug versions of games use fSELF which can be trivially "decrypted"
- The elf_parser tool handles fSELF automatically

**Option B: RPCS3 Decryption**
- RPCS3 can dump decrypted ELFs when loading a game
- Enable "Dump Executable" in RPCS3 debug settings
- Look for the decrypted file in RPCS3's cache directory

**Option C: ps3recomp's fSELF rebuilder**

Only for **debug / prototype** fSELF binaries, which are unencrypted — this
rebuilds the original ELF without keys. A retail `EBOOT.BIN` is encrypted and
needs Option A or B.

```bash
python tools/unfself.py EBOOT.BIN --output game/EBOOT.ELF
python tools/unfself.py EBOOT.BIN --info      # describe it, write nothing
```

### Required Files

| File | Location on Disc | Purpose |
|------|-----------------|---------|
| `EBOOT.BIN` | `PS3_GAME/USRDIR/EBOOT.BIN` | Main game executable |
| `PARAM.SFO` | `PS3_GAME/PARAM.SFO` | Game metadata (title, ID, version) |
| `*.sprx` | `PS3_GAME/USRDIR/*.sprx` | Additional game modules (if any) |
| `TROPHY.TRP` | `PS3_GAME/TROPDIR/*/TROPHY.TRP` | Trophy configuration |
| Game assets | `PS3_GAME/USRDIR/*` | Textures, models, audio, scripts |

---

## Phase 3: Analysis

### ELF Analysis

`elf_parser.py` writes JSON to **stdout** — it has no `--output`; redirect it.

```bash
mkdir -p analysis
python tools/elf_parser.py game/EBOOT.ELF --all      > analysis/elf_info.json
python tools/elf_parser.py game/EBOOT.ELF --imports  > analysis/imports.json
python tools/elf_parser.py game/EBOOT.ELF --exports  > analysis/exports.json
```

The flags select what to report:
- *(default)* — header, entry point, architecture
- `--imports` — imported functions (module + NID)
- `--exports` — exported symbols (if any)
- `--sections` / `--segments` / `--relocs`, or `--all` for everything

### Key Questions to Answer

1. **How many imports?** — fewer is better; 100-200 is typical
2. **Which modules?** — check coverage against ps3recomp's HLE modules
3. **Any unresolved NIDs?** — these need stubs or implementations
4. **Entry point?** — the address where execution starts
5. **TOC (Table of Contents)?** — the r2 value needed for data access
6. **PRX modules?** — does the game load additional .sprx files?
7. **Memory layout?** — where does code, data, and BSS live?

### NID Resolution

```bash
# Resolve the NIDs your imports dump listed (one hex NID per line):
python tools/nid_database.py --batch analysis/nids.txt --json > resolved.json
python tools/nid_database.py --lookup 0xAB8B4DA4        # or one at a time
```

Review the output for unresolved NIDs. For each one:
- Check RPCS3 for the function name
- Determine if it's critical or can be stubbed
- Add it to the game's stubs.cpp if needed

---

## Phase 4: Disassembly

### Function Detection

```bash
python tools/find_functions.py game/EBOOT.ELF --output analysis/functions.json
```

Review the function list:
- How many functions? (typical: 500–5000 for an indie game, 10000+ for AAA)
- Are there symbols? (debug builds have names; retail usually stripped)
- Any suspiciously large "functions"? (might be data misidentified as code)

### Full Disassembly

```bash
python tools/ppu_disasm.py game/EBOOT.ELF \
    --functions analysis/functions.json \
    --annotate \
    --output disasm/
```

### SPU Program Detection

If the game uses SPU programs:
```bash
python tools/extract_spu_images.py game/EBOOT.ELF --output spu_programs/
```

SPU ELF segments are embedded within the main PPU ELF. Each one needs separate analysis.

---

## Phase 5: Code Generation

### PPU Code Lifting

```bash
python tools/ppu_lifter.py disasm/ \
    --nid-db tools/nid_db.json \
    --output recomp/
```

This generates:
- `functions_NNNN.c` — batches of recompiled C functions
- `func_table.cpp` — maps guest address → host function pointer
- `data_segments.c` — initialized data as C arrays

### SPU Code Lifting (if applicable)

```bash
# spu_disasm writes to stdout; redirect it
python tools/spu_disasm.py spu_programs/spu_0.elf > spu_disasm/spu_0.txt
# SPU lifter generates similar C output for each SPU program
```

---

## Phase 6: Project Setup

### Create the Project

```bash
cp -r ps3recomp/templates/project/ my_game/
```

### Configure

Edit `my_game/config.toml`:

```toml
[input]
elf_path = "../game/EBOOT.ELF"

[modules]
# Enable only the modules your game uses
cellSysutil   = "hle"
cellGcmSys    = "hle"
cellPad       = "hle"
cellAudio     = "hle"
cellFs        = "hle"
cellFont      = "hle"
# ... add all modules from your import analysis

[debug]
log_hle_calls = true
log_missing_nids = true
break_on_unimplemented = true   # Stop on unimplemented calls
```

### Copy Generated Files

```bash
cp recomp/ppu_recomp.h my_game/recompiled/
cp recomp/*.c recomp/*.cpp my_game/recompiled/
cp data_segments.c my_game/recompiled/
```

`recompiled/` is what the template calls `RECOMP_DIR`, and `ppu_recomp.h` has
to be in it: the boot scaffold and every lifted source `#include` it, and the
build stops with a message rather than a wall of errors if it is missing. Keep
the lift somewhere else if you prefer and point at it instead:

```bash
cmake -B build -DRECOMP_DIR=/path/to/lift
```

### Set Up Game Assets

Create the virtual filesystem:

```bash
mkdir -p my_game/hdd0/game/TITLEID/USRDIR
mkdir -p my_game/hdd0/home/00000001/trophy/TITLEID
# Copy game assets
cp -r PS3_GAME/USRDIR/* my_game/hdd0/game/TITLEID/USRDIR/
```

---

## Phase 7: Building and Debugging

### First Build

```bash
cd my_game
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp
cmake --build build 2>&1 | tee build.log
```

Expect compilation errors on first try. Common issues:

**Undefined functions:**
```
error: undefined reference to 'recomp_func_00012345'
```
→ Function was referenced but not in the function list. Add it or add a stub.

**Type mismatches:**
```
error: incompatible pointer types
```
→ The lifter may need manual adjustment for complex calling conventions.

### First Run

The runner takes the PPU ELF on the command line -- the same one the lifter
read. It derives the virtual filesystem root from that path
(`<root>/PS3_GAME/USRDIR/EBOOT.ELF` gives `<root>`), so point it at the ELF
inside the game tree rather than at a copy somewhere else, or set
`PS3_VFS_ROOT`.

```bash
./build/MyGameRecomp game/PS3_GAME/USRDIR/EBOOT.ELF 2>&1 | tee run.log
```

Expected output:
```
=== ps3recomp game runner ===
[boot] VFS root: game
[rsx] Metal backend init OK -- window open

[boot] dispatching entry OPD 0x00010000 (stack top 0x0FF00000)

[hle] unresolved NID 0xABCDEF01
[LV2] unimplemented syscall 352 (0x160)
```

Every one of those lines is the next thing to implement. Execution runs real
guest code until it reaches a function outside the lifted subset, a firmware
import with no handler, or an lv2 syscall that has none -- each of which is
logged with what it was.

### Debugging Strategy

1. **Run under a debugger** (GDB/LLDB/Visual Studio)
2. **Set `break_on_unimplemented = true`** to catch missing functions immediately
3. **Compare with RPCS3** — run the same game in RPCS3 with logging and compare the call sequences
4. **Use log output** — `log_hle_calls = true` shows every HLE function call
5. **Binary search for crashes** — if the game crashes in recompiled code, narrow down the problematic function by checking the guest address from the backtrace

---

## Phase 8: Stubbing and Implementation

### Adding Game-Specific Stubs

In `stubs.cpp`, add implementations for unresolved NIDs:

```cpp
#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include <cstdio>

// Stub for a non-critical function
extern "C" int32_t cellFooDoSomething(uint32_t param1, uint32_t param2)
{
    printf("[STUB] cellFooDoSomething(0x%x, 0x%x)\n", param1, param2);
    return CELL_OK;
}

// Stub for a function that returns data
extern "C" int32_t cellBarGetInfo(uint32_t* out_info)
{
    if (out_info) *out_info = 0;
    printf("[STUB] cellBarGetInfo -> 0\n");
    return CELL_OK;
}
```

### Priority Order

1. **Functions that crash if missing** — stub immediately
2. **Functions that produce wrong behavior** — implement properly
3. **Functions called during init** — must at least return CELL_OK
4. **Functions called during gameplay** — implement for correctness
5. **Rarely-called functions** — stub last

---

## Phase 9: Graphics

### The Challenge

RSX graphics is the hardest part of any PS3 port. The game writes NV47xx GPU commands to a command buffer, and these need to be translated to a modern graphics API.

### Strategy

**Phase 1: Null renderer**
- Set `backend = "null"` in config
- Verify all game logic runs correctly without graphics
- This proves the recompilation and HLE are working

**Phase 2: State tracking**
- cellGcmSys already tracks RSX state (tiles, zcull, display buffers)
- Log all command buffer writes to understand the rendering pipeline
- Identify the vertex formats, shader programs, and draw call patterns

**Phase 3: Graphics backend**
- Implement a Vulkan or D3D12 renderer that consumes the command buffer
- Start with basic triangle rendering
- Progressively add shader translation, texture sampling, blending modes

### What's Implemented

**cellGcmSys** (HLE module — state management):
- Command buffer control (put/get pointers)
- Local memory allocation (VRAM heap)
- IO memory mapping (main memory → GPU accessible)
- Tile and zcull configuration
- Display buffer registration, flip and VBlank callbacks
- Timestamps and report data

**RSX Command Processor** (`libs/video/rsx_commands.h/.c`):
- NV47xx FIFO command buffer parsing (type 0 increasing, type 2 non-increasing)
- State tracking: surfaces, viewport, scissor, clear, blend, depth/stencil, cull, color mask, alpha test
- Texture state: 16 units with offset/format/address/filter/rect
- Vertex attributes: 16 attribs with format/offset/stride
- Shader program registers (fragment address, vertex load slot)
- Draw dispatch: draw_arrays, draw_indexed
- Backend callback interface (`rsx_backend`) with 12 dispatch points

**Null backend** (`libs/video/rsx_null_backend.h/.c`):
- Win32 window creation
- Displays RSX clear color via GDI
- FPS counter and debug overlay
- Proves command flow works before implementing a real backend

**What's NOT done yet:**
- D3D12/Vulkan backend (actual triangle rendering)
- Vertex/fragment program translation (RSX shaders → HLSL/SPIR-V)
- Texture upload and sampling
- Framebuffer management and resolve

---

## Phase 10: Audio and Input

### Audio

cellAudio provides real audio output. Most games just need:
1. `cellAudioInit()` — starts the mixing thread
2. Write PCM samples to port buffers
3. `cellAudioSetNotifyEventQueue()` — sync with the mixing interval

**Common issues:**
- Audio crackling → increase buffer size in config
- No sound → check that the audio backend matches your OS
- Wrong sample rate → PS3 native is 48000 Hz

### Input

cellPad provides real gamepad input via XInput (Windows) or SDL2.

**Button mapping (XInput → PS3):**
| XInput | PS3 |
|--------|-----|
| A | Cross |
| B | Circle |
| X | Square |
| Y | Triangle |
| LB | L1 |
| RB | R1 |
| LT | L2 (analog) |
| RT | R2 (analog) |
| Back | Select |
| Start | Start |
| Guide | PS |

---

## Phase 11: SPU Handling

### Assessment

Determine how the game uses SPUs:

1. **SPURS framework** — most games use this
   - Check for `cellSpursInitialize`, `cellSpursCreateTask` calls
   - The SPURS HLE handles management; actual SPU tasks need attention

2. **Raw SPU** — some games create SPU threads directly
   - Check for `sys_spu_thread_group_create` syscalls
   - Each SPU program needs separate analysis

3. **SPU task types** — identify what the SPU programs do:
   - **Audio mixing** (ATRAC3+, PCM mixing) → replace with host decoding
   - **Physics** (Havok, Bullet) → intercept and run on host CPU
   - **Decompression** (zlib, LZ) → replace with host zlib
   - **Vertex processing** → handle in graphics backend
   - **Custom compute** → most complex; may need SPU interpreter

### HLE Approach (Preferred)

If you can identify the SPU task type:

```cpp
// In stubs.cpp — intercept a known SPU audio mixing task
void hle_spu_audio_mixer(spu_context* ctx)
{
    // Read parameters from SPU local store
    uint32_t src_addr = spu_ls_read32(ctx, 0x100);
    uint32_t dst_addr = spu_ls_read32(ctx, 0x104);
    uint32_t num_samples = spu_ls_read32(ctx, 0x108);

    // Perform the mixing on the host CPU
    float* src = (float*)(vm_base + src_addr);
    float* dst = (float*)(vm_base + dst_addr);
    for (uint32_t i = 0; i < num_samples; i++)
        dst[i] += src[i];
}
```

---

## Phase 12: Testing and Polish

### Testing Checklist

- [ ] Game starts without crashes
- [ ] Main menu is functional
- [ ] Gameplay runs at correct speed
- [ ] Audio plays correctly
- [ ] Input is responsive
- [ ] Save data loads and saves
- [ ] Trophy unlocks work
- [ ] No memory leaks (check with AddressSanitizer/Valgrind)
- [ ] Performance is acceptable (profile with host profiling tools)

### Performance Tuning

1. **Compile with optimizations** — `-O2` or `/O2` makes a huge difference
2. **Profile** — use perf/VTune/Instruments to find hotspots
3. **Batch DMA** — if SPU DMA is a bottleneck, consider batching transfers
4. **Reduce logging** — disable `log_hle_calls` for release builds
5. **Memory access patterns** — the endian conversion can be a bottleneck; consider caching

---

## Platform Notes

A port started from `templates/project` builds and boots on Windows, macOS and
Linux as it stands. CI proves the last two on every push: it configures the
template with `PS3RECOMP_DIR` at the toolkit and `RECOMP_DIR` at a lift, builds
it, and runs it against the boot smoke title -- on headless Metal for macOS and
on the null backend for Linux.

### What the toolkit provides

You do not write a backend, a window or a shim. The runtime has them:

| Piece | Windows | macOS | Linux |
|---|---|---|---|
| Present backend | D3D12 | Metal | null (headless software) |
| Window and message pump | `rsx_*_backend_pump_messages` | same | same |
| FIFO walker | `cellGcm_rsx_process_fifo` | same | same |
| Win32 API (threads, sync, timing, `VirtualAlloc`) | the real thing | `runtime/platform/win32_compat.h` | same |
| Audio and pad | WASAPI, XInput | SDL2 | SDL2 |

`templates/project/main.cpp` selects the backend with an `#if` on the host and
calls the same three entry points either way, so the frame clock above them is
written once. `runtime/ppu/tests/boot_main.cpp` makes the same selection; if
you are reading one to understand the other, they agree.

### What a runner has to do itself

Most ports do not start from the template -- they start from a runner that grew
up on Windows. Four things stand between that and a build on another host, and
they are the same four every time:

**1. Separate the toolkit from the game.** One variable usually means both: the
tree that owns `include/`, `runtime/` and `libs/`, and the tree that owns the
lifter's output. They are only the same directory while the toolkit is
vendored. Split them -- `PS3RECOMP_DIR` and `RECOMP_DIR` in the template -- and
the runner can be built against a toolkit checkout with the generated code left
where it is.

**2. Gate the Windows-only build pieces.** These fail at configure time, before
a compiler is reached, so they come first:

- **Resource scripts.** `app.rc` needs a resource compiler. On a host without
  one it is not a link error, it is "no rule to make" for a file nothing can
  consume.
- **System import libraries.** `dbghelp`, `user32`, `gdi32`, `winmm` name
  nothing a POSIX linker can find. The D3D12 backend's own imports arrive
  through `#pragma comment(lib)` and need no CMake entry at all.
- **MSVC flags.** `/W3`, `/bigobj`, `/Zc:__cplusplus` under `if(MSVC)`, with a
  Clang/GCC branch beside it -- not an `if(MSVC)` with no `else`, which leaves
  hundreds of thousands of lines of generated code compiled at the default
  optimisation level with warnings on.
- **The link language.** Set it to CXX explicitly. Inferring it from the
  generated C leaves the C++ standard library out of the link.

**3. Take the Win32 names from the shim.** `#include <windows.h>` is the first
thing a build elsewhere trips over. `runtime/platform/win32_compat.h` supplies
the same names over pthreads and mmap -- the sync and timing API, the
interlocked operations, the scalar typedefs, `VirtualAlloc`, the vectored
exception handlers -- and on Windows it is a passthrough to `<windows.h>` with
`WIN32_LEAN_AND_MEAN` already set. Call sites do not move; the names arrive
from somewhere else. Include it early: anything using `EXCEPTION_POINTERS` or
`CONTEXT` needs it before the first use, not wherever `<windows.h>` happened to
sit.

The headers that have no shim and no meaning elsewhere stay under `_WIN32`:
`<timeapi.h>` for `timeBeginPeriod`, `<dbghelp.h>`, `<tlhelp32.h>`,
`<intrin.h>`. Each names something the shim already provides off Windows, so
the guard removes a header rather than a capability.

**4. Compile out or port the Win32 diagnostics.** A runner that has debugged a
real boot accumulates them: an unhandled-exception filter reading `CONTEXT.Rip`
(which is not the register name on arm64), a `tlhelp32` thread walk with
`SuspendThread`/`GetThreadContext`, symbolization through `SymFromAddr`. The
thread walk in particular has no POSIX equivalent that is not a debugger. Put
them under `_WIN32` and move on; port them only when a bug on the other host
needs them. `boot_main.cpp` does exactly this and is worth reading for where
the line falls.

### Things that only show up off Windows

- **arm64 has a weak memory model.** `volatile` was never a fence and on Apple
  Silicon it stops looking like one. Interlocked operations, not `volatile ++`.
  `docs/PLATFORM_ABSTRACTION.md` has the checklist.
- **Clang does not have MSVC's dialect.** `__declspec(thread)` is parsed and
  then discarded unless something maps it; `runtime/platform/msvc_compat.h`
  maps it to `__thread`, which is a real thread-local. A generated file
  compiled without that mapping references a thread-local as an ordinary
  global, and Mach-O does not diagnose it.
- **Which signal a bad access raises is per-OS.** Linux delivers an access to a
  mapped-but-forbidden page as `SIGSEGV`; Darwin turns the same one into
  `SIGBUS`. A handler for guest pointer faults has to take both, which is what
  `ppu_loader.cpp` does.
- **The default host thread stack is 512 KB on Darwin.** A recompiled call
  chain nests one host frame per guest call; Windows reserves 256 MB.
- **`-ffp-contract=off` on the lifted sources.** arm64 fuses a multiply and an
  add by default. A PPC `fmadd` is spelt as one where the lifter means one.

---

## Tips and Best Practices

1. **Start with RPCS3** — always verify game behavior in RPCS3 first
2. **Incremental approach** — get one screen working, then the next
3. **Version control everything** — commit early and often
4. **Document your findings** — each game has quirks; write them down
5. **Check RPCS3's game compatibility wiki** — known issues are documented
6. **Join the community** — other porters may have solved your problem
7. **Don't fight the lifter** — if generated code is wrong, fix the lifter, not individual functions (unless it's truly a one-off issue)
8. **Test on multiple platforms** — if you're only on Windows, test on Linux too (or vice versa)

---

## Case Study: flOw

The first game port actively in progress using ps3recomp. This section tracks real-world progress and lessons learned.

**Game:** flOw by thatgamecompany (NPUA80001)
**Engine:** PhyreEngine (Sony first-party)
**Repository:** [sp00nznet/flow](https://github.com/sp00nznet/flow)

### Why flOw?

- Small PSN title (~10 MB ELF, ~50 MB total)
- Simple gameplay (2D physics-based, no complex 3D)
- Works perfectly in RPCS3 (behavior reference available)
- Limited SPU usage (audio only, no physics/compute)
- 140 imports across 12 modules — excellent ps3recomp coverage

### Current Status (v0.4.0)

| Milestone | Status |
|-----------|--------|
| ELF analysis & import resolution | **Complete** — 140/140 NIDs resolved |
| Function boundary detection | **Complete** — 51,658 functions found (OPD + heuristic) |
| PPU code lifting | **Complete** — all 51,658 functions lifted to C++ |
| Build & link | **Complete** — 37 MB native x86-64 executable |
| CRT startup | **Working** — TLS init, mutex creation, enters game code |
| HLE module bridges | **7/12 real** — cellSysutil, cellGcmSys, cellAudio, cellPad, cellFs, cellSysmodule, sysPrxForUser |
| Game main loop | **In progress** — crashes in early init due to address sign-extension |
| Graphics (RSX → D3D12) | Not started |
| Audio (cellAudio → WASAPI) | **Wired** — real WASAPI backend via ps3recomp |
| Input (cellPad → XInput) | **Wired** — real XInput backend via ps3recomp |

### Key Technical Lessons

**1. TOC Save in Import Stubs**

The PPC64 ELF ABI requires saving the TOC (r2) to `sp+40` before inter-module calls. The lifter doesn't emit this instruction, so after an import stub call, the caller's `ld r2, 40(r1)` reads garbage. **Fix:** The `nid_dispatch()` function in `import_stubs.cpp` saves the current TOC:

```c
static void nid_dispatch(ppu_context* ctx, uint32_t nid, const char* name) {
    vm_write64((uint32_t)ctx->gpr[1] + 0x28, ctx->gpr[2]); /* save TOC */
    void* handler = ps3_resolve_func_nid(nid);
    if (handler) ((int64_t(*)(ppu_context*))handler)(ctx);
}
```

**2. HLE Bridge Pattern**

ps3recomp's HLE functions take explicit C parameters, but recompiled code passes everything through `ppu_context` GPRs. Bridge functions extract parameters per the PPC64 ABI (r3-r10 = args, r3 = return), translate guest pointers (`vm_base + addr`), and byte-swap struct output for big-endian guest memory:

```c
static int64_t bridge_cellVideoOutGetResolution(ppu_context* ctx) {
    uint32_t resId    = (uint32_t)ctx->gpr[3];
    uint32_t res_addr = (uint32_t)ctx->gpr[4];
    CellVideoOutResolution host_res;
    s32 rc = cellVideoOutGetResolution(resId, &host_res);
    if (rc == CELL_OK && res_addr) {
        vm_write16(res_addr,     host_res.width);   /* big-endian write */
        vm_write16(res_addr + 2, host_res.height);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}
```

**3. 32-bit vs 64-bit Address Space**

The PS3 uses 32-bit effective addresses in 64-bit GPRs. Arithmetic like `sp -= 0xA0` can sign-extend the result to 64 bits. The `vm_bridge.cpp` truncates addresses with `(uint32_t)addr`, but code that uses GPRs for address comparison or branching may need masking in the lifter.

**4. WASAPI COM GUIDs**

When linking cellAudio's WASAPI backend on MSVC, the COM interface GUIDs (`IID_IAudioClient`, etc.) aren't automatically defined. **Fix:** Define them manually in `cellAudio.c` using `DEFINE_GUID()` macros rather than relying on `uuid.lib`.

### Analysis Numbers

```
Binary:          EBOOT.elf (10.1 MB, ELF64 big-endian PowerPC64)
Functions found: 51,658 (36,827 OPD + 14,831 heuristic)
Functions lifted: 51,658 (100%)
Generated C++:   156 MB (3.14 million lines)
Native binary:   37 MB (x86-64 Windows)
Import libraries: 12 (cellSysutil, cellGcmSys, cellSysmodule, cellSpurs,
                      cellAudio, cellSync, cellNetCtl, sceNp, sys_net,
                      sys_io, sys_fs, sysPrxForUser)
Import functions: 140 (all resolved)
Empty stubs:     16,718 (branch targets not in function list)
Unlifted insns:  ~24,660 (VMX/AltiVec SIMD — needed for PhyreEngine)
```
