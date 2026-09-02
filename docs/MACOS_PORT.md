# macOS / Apple Silicon port: status and plan

_Where the tree stands on an arm64 Mac, what a title still needs to run there,
and the order to do it in. Companion to `docs/PLATFORM_ABSTRACTION.md`, which
documents the host shim itself._

## What CI proves today (`.github/workflows/macos.yml`, `macos-14`, arm64)

| | |
|---|---|
| Runtime library | builds, Debug and Release, verified arm64 |
| Lifter suites | all eight, including the 1300+ conformance cases compiled and executed with Apple Clang |
| SPU helper tests | pass |
| `ps3recomp_host` | clear, flip, a fixed-function NV4097 draw, a draw through **guest vertex and fragment programs**, and a draw sampling a **guest texture** from a guest program -- headless through the **Metal** backend, with 60-frame runs |
| Guest shader translation | `test_shader_msl`: hand-assembled NV40 programs through the decompilers, glslang and spirv-cross, checked for the MSL binding contract the backend relies on (runs on Linux too) |
| PPU boot scaffold | `ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `boot_main.cpp` compile at a recorded baseline of **0** errors (`darwin-clang` in `tools/ppu_scaffold_baseline.json`) |
| Win32 host shim | `runtime/platform/tests/test_win32_compat.c`, 197 checks |
| lv2 sync primitives | `tests/sync_stress`: mutex, cond, semaphore and event-queue stress on host threads |
| RSX self-contained tests | texture layout and primitive tables |

The scaffold check is compile-only: nothing in this repository links it,
because that needs a lifted game.

## What a title still needs

1. **A renderer.** The production draw path is `libs/video/rsx_live_draw.c`,
   the live NV4097 draw engine, and it is D3D12-only: off Windows the whole file
   compiles to a stub (`#if !defined(_WIN32)`). The Metal backend
   (`rsx_metal_backend.m`) is the vtable-style backend. It runs the guest's
   own vertex and fragment programs -- decompiler HLSL, lowered to MSL by
   glslang and spirv-cross (`rsx_shader_msl.cpp`) and compiled at runtime --
   with per-draw constant blocks, the alpha test, culling and the colour
   mask, and it binds guest textures through the shared layout/decode path
   with the `TEXTURE_CONTROL1` crossbar as the texture's swizzle and the
   sampler registers decoded; every one of those semantics is copied field
   for field from the D3D12 backend. It has no depth/stencil attachment,
   no render-to-texture or MRT, no surface formats beyond the drawable's,
   no mip levels, cube maps or vertex textures. And it is the simpler
   engine: a title on Windows runs through `rsx_live_draw.c`, so the last
   step is either a seam under that file or the vtable backend growing the
   rest of that behaviour.

2. **A game's host code.** The scaffold in `runtime/ppu/` is game-agnostic; a
   port adds its own runner (imports, overrides, the window, diagnostics). A
   runner written against Win32 compiles against `runtime/platform/` first and
   is then ported where the shim stops: `SuspendThread` of a running thread,
   vectored exception handlers, `SymFromAddr`, `VirtualQuery`, the Win32 window
   and message pump.

3. **Time on the hardware.** Everything above is proven on GitHub's arm64
   runners. The 30-minute soaks, the frame-rate numbers and the memory-model
   bugs only show up on a real machine with a real title.

## Order of work

### 1. Renderer

The Metal-native route was taken. `rsx_metal_backend.m` runs guest programs
through `libs/video/rsx_shader_msl.cpp` -- HLSL to SPIR-V with glslang,
SPIR-V to MSL with spirv-cross, both optional at configure time and the
backend fixed-function without them -- and binds guest textures through the
shared `rsx_texture_layout` path. There is no second emitter: the decompilers'
HLSL is the one source, and `test_shader_msl` checks on every push, on Linux
as well, that glslang's front end still accepts what they emit and that the
MSL carries the buffer, texture, sampler and attribute slots the backend binds
to. The alternative -- Vulkan through MoltenVK under a seam in
`rsx_live_draw.c`, with a Windows A/B through `rsx_live_replay` -- remains the
obvious shape for a Linux backend.

What is left, in order:

- **Depth and stencil.** A depth attachment on the drawable and the NV4097
  depth/stencil state as pipeline state (`set_depth_stencil` is still
  unhandled).
- **Render targets.** `SET_SURFACE_*` beyond the clip size: colour and zeta
  surfaces as textures, render-to-texture, MRT, surface formats.
- **The rest of texturing.** Mip levels, cube maps, vertex textures, and the
  formats `rsx_texture_layout` does not classify yet.
- **The production engine.** A seam under `rsx_live_draw.c`, or the vtable
  backend growing its behaviour. The game-proven pieces to carry across are in
  the Yakuza port's `rsx_live_draw.c`, `rsx_dispatch.c`, `rsx_gpu_mirror.c`
  and `rsx_guest_pages.c`.

Interim visibility before that lands: an SDL2 window that blits
`rsx_live_draw_present_rgba()` frames, so a Mac boot is visible.

### 2. Bring up a title

Build a game against the scaffold on macOS. The lifter output is portable C;
the pieces to expect trouble from are the ones the Windows build never
exercised: the `#else` branches in `runtime/syscalls/sys_*`, `cellSpurs.c`,
`cellFiber.c` and the audio/pad SDL2 backends. Compare each POSIX branch
against its Windows twin for the wait/repark, priority and timeout behaviour
added since; do not assume equivalence.

### 3. arm64 hardening (overlaps everything above)

The checklist is in `docs/PLATFORM_ABSTRACTION.md`, "Apple Silicon Notes".
The two that bite: `volatile` used as synchronisation in host runtime code --
it was never a fence, and on arm64 it stops looking like one -- and any
"data stores, then flag store" without a release/acquire pair. Do not fix an
arm64 hang by sprinkling fences: classify it against an x86-64 build under
Rosetta 2 (TSO, 4 KB pages), then trace producer, publication, ordering, wake
and consumer.

## Building and testing on a Mac

```bash
brew install cmake ninja sdl2 python@3.12 glslang spirv-cross
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --draw --frames=60   # fixed-function draw
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --shader             # guest VP + FP
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --tex                # guest texture via a guest FP
./build/test_shader_msl -v                        # decompilers -> MSL, sources printed

for t in tools/test_*.py; do python3 "$t"; done          # lifter suites
python3 tools/check_ppu_scaffold.py                     # scaffold ratchet
clang -std=gnu17 -I runtime/platform -o /tmp/twc \
      runtime/platform/tests/test_win32_compat.c runtime/platform/win32_compat.c && /tmp/twc
cmake -S tests/sync_stress -B build-ss -G Ninja && cmake --build build-ss && ./build-ss/sync_stress
```

Two switches on the backend: `PS3RECOMP_METAL_SHADER_DUMP=<dir>` writes every
translated program's HLSL and MSL there, named by cache key, and
`PS3RECOMP_METAL_FIXED_FUNCTION=1` pins every draw to the built-in shader --
the first thing to flip when a title's draws vanish, to tell a translation
problem from a fetch or state one.

Profiling moves from WPR/uProf to Instruments (`xctrace`) and `sample`. An
M1 Pro has 8 (or 6) performance cores and 2 efficiency cores; count busy host
threads before reading a frame rate, and keep the PPU, SPU-worker and RSX
threads at above-normal priority so QoS keeps them on the P-cores.
