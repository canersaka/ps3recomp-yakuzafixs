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
| `ps3recomp_host` | clear, flip, a fixed-function NV4097 draw, a draw through **guest vertex and fragment programs**, a draw sampling a **guest texture** from a guest program, a pair of triangles the **depth test** has to order, a two-level texture whose **second mip level** must be the one sampled, and a triangle drawn into an offscreen **colour surface** that a later draw then **samples** -- headless through the **Metal** backend, with 60-frame runs |
| Guest shader translation | `test_shader_msl`: hand-assembled NV40 programs through the decompilers, glslang and spirv-cross, checked for the MSL binding contract the backend relies on (runs on Linux too) |
| PPU boot scaffold | `ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `boot_main.cpp` compile at a recorded baseline of **0** errors (`darwin-clang` in `tools/ppu_scaffold_baseline.json`) |
| PPU boot path | `ps3recomp_boot_smoke`: the scaffold **linked and run** against the synthetic title in `runtime/ppu/tests/smoke/` -- ELF load, entry OPD, TLS, the NID bridge, lv2 syscalls, a guest thread on a second host thread, three frames cleared and flipped through the FIFO to the Metal backend, and `sys_process_exit` |
| Win32 host shim | `runtime/platform/tests/test_win32_compat.c`, 298 checks |
| lv2 sync primitives | `tests/sync_stress`: mutex, cond, semaphore and event-queue stress on host threads |
| RSX self-contained tests | texture layout and primitive tables |

The scaffold check is compile-only. What links and runs it is the smoke title:
a hand-written program in the lifter's ABI, with an image
(`tools/make_smoke_elf.py`) the loader reads for real, so the boot path is
exercised on the Mac without a game. See docs/PPU_RECOMP.md.

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
   for field from the D3D12 backend. Texturing is complete enough for a
   title's usual bindings: the formats the live draw engine handles, whole
   mip chains with the sampler's mip filter and LOD range, cube maps as six
   faces with the fragment program compiled to sample a direction, and the
   four vertex-texture units bound to the vertex stage so a transform
   program's `TXL` samples. It has one depth/stencil attachment
   shared by the display, the NV4097 depth and stencil state per draw, and
   clears as records in the frame's ordered stream, so a clear in the middle
   of a frame opens a new pass rather than being lost. It renders into the
   guest's own colour surfaces: `SET_SURFACE_COLOR_TARGET` picks A, B or an
   MRT set, offsets `cellGcmSetDisplayBuffer` never registered are offscreen
   surfaces in a registry keyed by raw offset, in the float formats
   `SET_SURFACE_FORMAT` names, each with its own zeta buffer, and a texture
   unit bound at a surface's offset samples that surface rather than guest
   memory. What it does not have: MRT targets B, C and D are attached and
   cleared but never written, because the fragment decompiler emits one
   `SV_TARGET`; a vertex-texture unit bound at a surface's offset still
   uploads from guest memory; there is no stencil write mask or two-sided
   stencil (neither register is decoded upstream), and no anisotropy or
   sampler LOD bias. And it is the simpler engine: a title on Windows runs
   through `rsx_live_draw.c`, so the last step is either a seam under that
   file or the vtable backend growing the rest of that behaviour.

2. **A game's host code.** The scaffold in `runtime/ppu/` is game-agnostic; a
   port adds its own runner (imports, overrides, the window, diagnostics). A
   runner written against Win32 compiles against `runtime/platform/`, which now
   covers the diagnostic surface a runner is built on as well as the
   synchronisation one: `SuspendThread` of a running thread with
   `GetThreadContext`, `VirtualQuery` and `IsBadReadPtr`, vectored exception
   handlers, `SymFromAddr`. What is still Windows-only there is the window and
   the message pump, `__try`/`__except`, and the x86 debug registers a hardware
   watchpoint needs. `docs/PLATFORM_ABSTRACTION.md` has the call-by-call table.

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

- **The rest of MRT.** Targets B, C and D are bound and cleared, and the
  pipeline mirrors target A's blend and colour mask onto them, but the
  fragment decompiler emits a single `SV_TARGET` so nothing is written to
  them. A deferred pass therefore fills its first G-buffer plane and no more.
  The work is in the decompiler, not the backend.
- **The rest of texturing.** What is left is narrow: anisotropic filtering,
  the LOD bias in `SET_TEXTURE_FILTER` (Metal has no sampler-side bias, so it
  means patching the sampling call in the fragment program), 3D textures, and
  `HILO_S8`'s signed channels.
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
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --depth              # near-first pair, depth test
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --mip                # two mip levels, level 1 sampled
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --rtt                # render into a surface, then sample it
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_boot_smoke build/smoke/smoke.elf   # the PPU boot path
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
