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
| `ps3recomp_host` | clear, flip, a fixed-function NV4097 draw, a draw through **guest vertex and fragment programs**, a draw sampling a **guest texture** from a guest program, a pair of triangles the **depth test** has to order, a two-level texture whose **second mip level** must be the one sampled, a triangle drawn into an offscreen **colour surface** that a later draw then **samples**, a **polygon and a quad strip** that only exist once expanded into triangles, and a **depth surface** one pass writes and the next samples as a texture -- headless through the **Metal** backend, with 60-frame runs, and **twice over**: once through the register-file draw engine, which is the default, and once through the older vtable path. The depth-texture mode is the one exception, and runs on the draw engine alone: the vtable path tracks no zeta as a texture |
| Guest shader translation | `test_shader_msl`: hand-assembled NV40 programs through the decompilers, glslang and spirv-cross, checked for the MSL binding contract the backend relies on (runs on Linux too) |
| PPU boot scaffold | `ppu_loader.cpp`, `ppu_hle.cpp`, `ppu_fs.cpp`, `ppu_sysprx.cpp`, `boot_main.cpp` and `templates/project/main.cpp` compile at a recorded baseline of **0** errors (`darwin-clang` in `tools/ppu_scaffold_baseline.json`) |
| PPU boot path | `ps3recomp_boot_smoke`: the scaffold **linked and run** against the synthetic title in `runtime/ppu/tests/smoke/` -- ELF load, entry OPD, TLS, the NID bridge, lv2 syscalls, a guest thread on a second host thread, three frames cleared and flipped through the FIFO to the Metal backend, and `sys_process_exit` |
| Game project template | `templates/project` configured with `PS3RECOMP_DIR` at the checkout and `RECOMP_DIR` at a lift, built with its own CMakeLists, and **run** against the smoke title -- the same verdict, through the runner a port actually starts from |
| Win32 host shim | `runtime/platform/tests/test_win32_compat.c`, 299 checks: threads, sync, `VirtualAlloc`/`VirtualQuery`, `SuspendThread` of a running thread with `GetThreadContext`, vectored exception handlers, `SymFromAddr` |
| lv2 sync primitives | `tests/sync_stress`: mutex, cond, semaphore, event-queue and rwlock stress on host threads, including recursive-mutex re-entry and write-lock ownership |
| lv2 threads | `ps3recomp_host --threads`: a guest PPU thread created and joined through the syscalls, and the host stack it got |
| cellFiber | `tests/fiber_switch`: 4000 scheduler round trips and 4000 direct fiber-to-fiber switches on ucontext (runs on Linux too) |
| Audio and pad | `ps3recomp_host --audio-pad`: the SDL2 backends up and down twice with `SDL_AUDIODRIVER=dummy` and no controller |
| RSX self-contained tests | texture layout, the primitive tables, and `test_rsx_draw_engine`: the draw engine driven through its dispatch sink against a stub backend, with no GPU -- surface keying and reallocation, render-target aliasing on a texture bind, an MRT set registered, cleared and bound whole, the texture cache's revalidation and LRU eviction, the pipeline key's stability under constant changes, restart cuts bounding topology expansion with quad strips and polygons among them (runs on Linux too) |

The scaffold check is compile-only. What links and runs it is the smoke title:
a hand-written program in the lifter's ABI, with an image
(`tools/make_smoke_elf.py`) the loader reads for real, so the boot path is
exercised on the Mac without a game. See docs/PPU_RECOMP.md.

## What a title still needs

1. **A renderer.** There are two on macOS, and the default is now the
   **register-file draw engine**: `libs/video/rsx_draw_engine.c` implements
   `rsx_dispatch_sink` over the whole NV4097 register file and drives
   `rsx_metal_backend.m` through `rsx_draw_backend`, a record-oriented
   interface of resources, pipelines and per-draw bindings with no host API
   in it. `PS3RECOMP_RSX_ENGINE=vtable` selects the older path, the one
   driven by `rsx_state`, and CI runs every host mode both ways.

   The engine's logic is carried across from `libs/video/rsx_live_draw.c`,
   the NV4097 to D3D12 engine a title has shipped on; that file is vendored
   and is not touched, so it keeps merging with upstream. What came across:
   vertex fetch through `rsx_vertex_compact` with **primitive restart** cuts
   and topology rebuilt into an indexed triangle list whose strip winding
   alternates per segment; a pipeline key over the vertex program's bytes,
   the fragment program's structure, `SHADER_CONTROL`'s export bit, the cube
   and vertex-texture masks, the input layout and the structural render
   state, with negative results cached; a colour surface set keyed by
   **(context DMA, offset)**, reallocated on a size or format change and
   seeded from the guest's own bytes; **one depth target per zeta address**;
   surface aliasing, so a texture unit that names a render target's address
   samples the live target; depth-as-texture behind the had-write contract;
   a texture cache keyed on where the bytes are and what the registers say,
   revalidated by content hash once per frame and evicted least-recently-used
   under pressure; the transform constant block with the viewport epilogue
   and the buffered fragment constants; the guest scissor intersected with
   the surface; the stencil reference kept dynamic; the FP16 HDR surface
   format; and a flip that resolves a display buffer id to a **registered**
   surface rather than to whatever happens to be bound. Indexed draws are
   really indexed, through an index buffer, and points and lines are drawn
   as the guest issued them.

   Both paths run the guest's own vertex and fragment programs -- decompiler
   HLSL, lowered to MSL by glslang and spirv-cross (`rsx_shader_msl.cpp`) and
   compiled at runtime -- with the alpha test, culling and the colour mask,
   and both bind guest textures through the shared layout/decode path with
   the `TEXTURE_CONTROL1` crossbar as the texture's swizzle and the sampler
   registers decoded. Texturing is complete enough for a title's usual
   bindings: the formats the live draw engine handles, whole mip chains with
   the sampler's mip filter and LOD range, cube maps as six faces with the
   fragment program compiled to sample a direction, and the four
   vertex-texture units bound to the vertex stage so a transform program's
   `TXL` samples.

   An MRT set is attached and cleared whole on both paths now, and quad
   strips and polygons are expanded into the engine's indexed triangle list
   the way the vtable path expands them, so those two differences are gone.
   Depth-as-texture has a check: `--depthtex` writes a zeta and samples it.

   What the engine does **not** have. Nothing writes colour targets B, C and
   D on either path, because the fragment decompiler emits one `SV_TARGET`;
   they are attached and cleared, not fed. A vertex-texture unit bound at a
   surface's offset still uploads from guest memory. A colour target is
   seeded only when its context DMA says main memory and the IO table says
   the page is mapped; a VRAM surface is not seeded, because this tree's
   guest VM reserves local memory without backing it. A guest program pair
   that will not translate drops its draw rather than falling back, as the
   reference engine drops it, so `PS3RECOMP_METAL_FIXED_FUNCTION=1` is a
   vtable-path lever now. And nothing carries across the movie compositor,
   the a010 probe, the shader disk cache or the profiling instrumentation,
   none of which a port wants.

2. **A game's host code.** The scaffold in `runtime/ppu/` is game-agnostic; a
   port adds its own runner (imports, overrides, the window, diagnostics). A
   runner written against Win32 compiles against `runtime/platform/`, which now
   covers the diagnostic surface a runner is built on as well as the
   synchronisation one: `SuspendThread` of a running thread with
   `GetThreadContext`, `VirtualQuery` and `IsBadReadPtr`, vectored exception
   handlers, `SymFromAddr`. What is still Windows-only there is the window and
   the message pump, `__try`/`__except`, and the x86 debug registers a hardware
   watchpoint needs. `docs/PLATFORM_ABSTRACTION.md` has the call-by-call table.

   What a runner needs from the toolkit is largely there. The window, the
   present surface and the message pump exist in the Metal backend; a FIFO
   walker exists in `cellGcmSys.c` and already feeds both render models; the
   threading a runner is written against is the shim. What the Yakuza runner
   keeps behind Win32 is, on inspection, four exception handlers that are all
   diagnostics (page-guard watchpoints and a crash reporter) plus the null
   backend's Win32 window, none of which a first Mac boot needs. Its build
   graph is the real precondition: it builds against its own vendored copy of
   the toolkit, which has no `runtime/platform/` and no Metal backend, so it
   has to be pointed at this tree first.

3. **Time on the hardware.** Everything above is proven on GitHub's arm64
   runners. The 30-minute soaks, the frame-rate numbers and the memory-model
   bugs only show up on a real machine with a real title.

## Order of work

### 1. Renderer

The Metal-native route was taken. `rsx_metal_backend.m` runs guest programs
through `libs/video/rsx_shader_msl.cpp` -- HLSL to SPIR-V with glslang,
SPIR-V to MSL with spirv-cross, both optional at configure time -- and binds
guest textures through the shared `rsx_texture_layout` path. Without those two
the vtable path falls back to a built-in shader and the draw engine has no
pipeline to build at all, so a build meant to render needs them. There is no
second emitter: the decompilers' HLSL is the one source, and `test_shader_msl`
checks on every push, on Linux as well, that glslang's front end still accepts
what they emit and that the MSL carries the buffer, texture, sampler and
attribute slots the backend binds to.

A Linux backend now has a shape to fill in rather than a design question:
`rsx_draw_backend` is where Vulkan (or Vulkan through MoltenVK) would go, and
`rsx_draw_engine.c` above it is already built and tested on Linux.

The production engine question is settled, and the answer was neither of the
two it was posed as. There is **no seam under `rsx_live_draw.c`**: that file
is vendored, upstream grew it from 1,566 to 8,453 lines in seven weeks, only
about 7% of it is D3D12, and a third of it is diagnostics and one title's
private behaviour that a port does not want. It cannot even be initialised
from this repository, so a refactor of it could not be validated here on any
platform. Instead the macOS path drives `rsx_dispatch`'s register file
through `libs/video/rsx_draw_engine.c`, which carries the proven engine logic
across into platform-neutral C over a backend interface `rsx_metal_backend.m`
implements. `rsx_live_draw.c` keeps working on Windows and keeps merging.

What is left, in order:

- **MRT.** Both paths now attach and clear every target the set names, so
  half of it is done. What is left is the fragment decompiler, which emits a
  single `SV_TARGET`: until it emits more, a deferred pass still fills its
  first G-buffer plane and no other, over a correctly cleared one.
- **The rest of texturing.** What is left is narrow: anisotropic filtering,
  the LOD bias in `SET_TEXTURE_FILTER` (Metal has no sampler-side bias, so it
  means patching the sampling call in the fragment program), 3D textures, and
  `HILO_S8`'s signed channels.
- **The production engine: decided.** No seam under `rsx_live_draw.c`.
  Measured, that file is 6.8% D3D12 by line; the rest is engine logic,
  diagnostics and one title's features. It grew by about 145 lines a day
  upstream over the summer, and nothing in this repository initialises or
  tests it, so a refactor of it could neither be validated here nor kept
  merging. The register-file draw engine above is the answer, and the
  vendored file keeps merging with upstream untouched.
- **Time with a title.** Everything above is a known gap. What a real
  frame turns up will not be.

### 2. Bring up a title

Build a game against the scaffold on macOS. The lifter output is portable C;
the pieces to expect trouble from are the ones the Windows build never
exercised: the `#else` branches in `runtime/syscalls/sys_*`, `cellSpurs.c`,
`cellFiber.c` and the audio/pad SDL2 backends. Compare each POSIX branch
against its Windows twin for the wait/repark, priority and timeout behaviour
added since; do not assume equivalence.

**The runner is not one of them.** `templates/project` is a working runner on
this platform, built and run against the smoke title in CI, and a new port
starts there rather than from a Windows-shaped file:

```bash
cp -r templates/project my_game
cmake -S my_game -B my_game/build -G Ninja \
      -DPS3RECOMP_DIR=/path/to/ps3recomp \
      -DRECOMP_DIR=/path/to/lift
cmake --build my_game/build
./my_game/build/MyGameRecomp game/PS3_GAME/USRDIR/EBOOT.ELF
```

The toolkit supplies the window, the message pump, the FIFO walker and the
Win32 shim, so what the runner owns is small: the backend `#if`, the frame
clock, and its own diagnostics. `main.cpp` is that, and `boot_main.cpp` is the
same boot with a real title's instrumentation on it.

An **existing Windows runner** takes four mechanical steps to reach the same
point, and they are worth doing in this order because the first two fail before
a compiler is reached:

1. **Point it at the toolkit.** One variable usually names both the toolkit and
   the game tree, which works only while the toolkit is vendored in the port.
   Split them, and the runner builds against a checkout with its generated code
   left where it is.
2. **Gate the Windows-only build pieces.** `app.rc` needs a resource compiler.
   `dbghelp`, `user32`, `gdi32` and `winmm` name nothing a POSIX linker can
   find. The lifted chunks want a Clang branch beside the `if(MSVC)` one, or
   400k-line generated files compile unoptimised with warnings on. Set the link
   language to CXX explicitly: inferred from the generated C, it leaves the C++
   standard library and the Objective-C runtime out.
3. **Take the Win32 names from `runtime/platform/win32_compat.h`** instead of
   `<windows.h>`, and include it early -- before anything that uses
   `EXCEPTION_POINTERS` or `CONTEXT`. `<timeapi.h>`, `<dbghelp.h>`,
   `<tlhelp32.h>` and `<intrin.h>` stay under `_WIN32`; each names something
   the shim or `win32_backtrace.h` already provides here, so the guard removes
   a header and not a capability.
4. **Compile out the Win32 diagnostics.** The crash filter reads `CONTEXT.Rip`,
   which is not the register on arm64; the hang watchdog's `tlhelp32` walk with
   `SuspendThread`/`GetThreadContext` has no POSIX equivalent short of writing
   a debugger. Under `_WIN32` they go, and they get ported when a bug here
   actually needs them. `boot_main.cpp` draws that line already -- its frame
   clock, which the boot depends on, is deliberately outside the guard while
   the watchdog is inside it.

`docs/GAME_PORTING_GUIDE.md`'s platform notes carry the same recipe with the
Linux column beside it.

A first pass over those branches has been made and what it found is fixed:
the rwlock had no write-lock owner off Windows, so a foreign unlock released
somebody else's lock; a recursive mutex re-entered through `trylock` never
took the host mutex, so the first guest unlock let another thread in;
`cellFiber` defined `_XOPEN_SOURCE` after its includes, which on Darwin
leaves `ucontext_t` the 64-byte header while `getcontext` writes 816 bytes
into it, and its fiber-to-fiber switch saved into the scheduler's context
rather than the calling fiber's; guest PPU threads took the default host
stack, 512 KB here against the 256 MB Windows reserves; and the three
`cellSpurs` host threads -- kernel poll, policy-module workers, job-chain
walker -- were inside `#ifdef _WIN32` with no `#else`, so SPURS initialised
and then did nothing at all. Each has a check in the table above except the
last, which needs a title to have anything to run.

The SDL2 audio and pad backends were the other unknown and they were fine as
written: init, poll and shutdown complete on CoreAudio and on the dummy
driver, with and without a controller.

What is left there is what a title has to settle. The known differences that
were left alone are the ones where POSIX is already the stricter branch --
`pthread_cond_timedwait` honours a microsecond deadline, so the POSIX side
has no equivalent of the sub-millisecond poll loops the Win32 branches carry
to work around a 15.6 ms timer -- plus the diagnostic levers (`FLOW_CONDKICK`,
`YDKJ_THREADGATE`, the `POKESEM`/`UNSTICK` semaphore pokes, `GetThreadTimes`
CPU accounting) that exist only on Windows and only under an environment
variable.

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
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --quads              # a polygon and a quad strip, expanded
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --depthtex           # write a zeta, then sample it (draw engine only)
PS3RECOMP_RSX_ENGINE=vtable PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --rtt  # ... through the older path
PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_boot_smoke build/smoke/smoke.elf   # the PPU boot path
./build/ps3recomp_host --threads                  # lv2 thread create/join + host stack
SDL_AUDIODRIVER=dummy ./build/ps3recomp_host --audio-pad   # SDL2 audio + pad up and down
./build/test_shader_msl -v                        # decompilers -> MSL, sources printed

clang -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
      -o /tmp/tde libs/video/tests/test_rsx_draw_engine.c \
      libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c \
      libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c \
      libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c && /tmp/tde

for t in tools/test_*.py; do python3 "$t"; done          # lifter suites
python3 tools/check_ppu_scaffold.py                     # scaffold ratchet
clang -std=gnu17 -I runtime/platform -o /tmp/twc \
      runtime/platform/tests/test_win32_compat.c runtime/platform/win32_compat.c && /tmp/twc
cmake -S tests/sync_stress -B build-ss -G Ninja && cmake --build build-ss && ./build-ss/sync_stress
cmake -S tests/fiber_switch -B build-fb -G Ninja && cmake --build build-fb && ./build-fb/fiber_switch
```

Switches on the render path:

- `PS3RECOMP_RSX_ENGINE=vtable` goes back to the `rsx_state` vtable path;
  `=dispatch` is the default and asks for the register-file draw engine
  explicitly. Every host mode above passes both ways, which is what the
  second CI block checks -- except `--depthtex`, which is the draw engine's:
  the vtable path has no depth-as-texture, so it uploads the guest bytes
  behind the zeta and presents the wrong pixel.
- `PS3RECOMP_METAL_SHADER_DUMP=<dir>` writes every translated program's HLSL
  and MSL there, named by cache key.
- `PS3RECOMP_METAL_FIXED_FUNCTION=1` pins every draw to the built-in shader,
  the first thing to flip when a title's draws vanish, to tell a translation
  problem from a fetch or state one. On the draw engine it disables the
  translator outright and nothing draws, so pair it with
  `PS3RECOMP_RSX_ENGINE=vtable`.

Profiling moves from WPR/uProf to Instruments (`xctrace`) and `sample`. An
M1 Pro has 8 (or 6) performance cores and 2 efficiency cores; count busy host
threads before reading a frame rate, and keep the PPU, SPU-worker and RSX
threads at above-normal priority so QoS keeps them on the P-cores.
