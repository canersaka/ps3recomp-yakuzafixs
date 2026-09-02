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
| `ps3recomp_host` | clear, flip, a real NV4097 draw and a 60-frame run through the **Metal** backend, headless |
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
   (`rsx_metal_backend.m`) is the vtable-style backend: clear, flip, a
   fixed-function draw with a built-in shader and a PSO cache -- enough for
   `ps3recomp_host`, not for a game. Nothing runs guest shaders on a Mac yet.

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

Two routes, and the recommendation is the first:

- **Vulkan through MoltenVK.** Put a small seam under the ~50 functions of
  `rsx_live_draw.c` that touch D3D12 (device, swap chain, PSO, resources,
  command lists, present) and implement it on Vulkan, with SDL2 owning the
  window and surface. The HLSL the fp/vp decompilers emit stays as it is and is
  compiled to SPIR-V at cache-miss time (glslang's HLSL front end; DXC for
  what it rejects). The same backend serves Linux, and on Windows it gives an
  A/B against D3D12 through the existing `rsx_live_replay` capture harness --
  which is how the port gets validated without a Mac-side oracle.
- **Metal-native.** Extend `rsx_metal_backend.m` with the guest shader path
  (HLSL -> SPIR-V -> MSL via spirv-cross). Fewer layers on a Mac; no Linux, no
  Windows A/B, and a second emitter to keep in step with the first.

Interim visibility before either lands: an SDL2 window that blits
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
brew install cmake ninja sdl2 python@3.12
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --draw --frames=60

for t in tools/test_*.py; do python3 "$t"; done          # lifter suites
python3 tools/check_ppu_scaffold.py                     # scaffold ratchet
clang -std=gnu17 -I runtime/platform -o /tmp/twc \
      runtime/platform/tests/test_win32_compat.c runtime/platform/win32_compat.c && /tmp/twc
cmake -S tests/sync_stress -B build-ss -G Ninja && cmake --build build-ss && ./build-ss/sync_stress
```

Profiling moves from WPR/uProf to Instruments (`xctrace`) and `sample`. An
M1 Pro has 8 (or 6) performance cores and 2 efficiency cores; count busy host
threads before reading a frame rate, and keep the PPU, SPU-worker and RSX
threads at above-normal priority so QoS keeps them on the P-cores.
