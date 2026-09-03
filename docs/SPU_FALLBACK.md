# Running SPU threads

`sys_spu_thread_group_start` has three ways to run an SPU thread, and it
tries them in this order:

1. **Lifted SPU code** -- the image's own program, recompiled to C by the
   SPU lifter and registered with `spu_register_function`. This is the
   faithful path and the one a title with a SPURS kernel needs. See
   [Starting a thread on lifted code](#starting-a-thread-on-lifted-code).
2. **A PPU fallback** -- a host-side stand-in registered for the image's
   entry point, for a job whose *output* you can produce without running
   its code. See [PPU fallback API](#ppu-fallback-api).
3. **The interpreter** -- `RD_SPU_INTERP=1` interprets an image that has
   neither of the above, straight out of local store.

A thread whose image matches none of the three completes instantly with
status 0. For many games that is fine: a lot of SPU work produces side
effects nothing depends on, and the PPU code is happy once the group
reports "all threads exited cleanly". It is not fine for a game that needs
real SPU output -- PhyreEngine asset decompressors, audio mixers, particle
simulations, physics, and anything downstream of a SPURS kernel -- because
stubbing those leaves PPU code reading garbage, or waiting forever.

In all three cases the thread runs on a host thread of its own, so a group
has real concurrency, and `sys_spu_thread_group_join` blocks until every
one of them is done.

## Starting a thread on lifted code

`sys_spu_thread_group_start` asks the lifted-code registry whether it has a
function at the thread's entry point (the entry the `sys_spu_image` at
`img_ea+4` declares, which `_sys_spu_image_import` filled in from the ELF).
If it does, that thread runs the real program.

### Registering an image

The lifter emits a `spu_recomp_register()` per image. A title calls
`spu_begin_image(id)` first, so every function that image registers is
keyed on `(LS address, image id)` rather than the address alone:

```c
spu_begin_image(16);  spu_recomp_register_kernel();   /* SPURS kernel */
spu_begin_image(2);   spu_recomp_register_policy();   /* taskset policy module */
spu_begin_image(3);   spu_recomp_register_gstask();   /* a geometry task */
spu_begin_image(0);                                   /* back to the wildcard */
```

The ids matter because SPURS loads its kernel, its policy modules and its
job binaries at **overlapping local-store addresses** at different times.
Image 0 is a wildcard that matches anything, which is right for a title
with one image and wrong for a title with several: a lookup that falls back
to it can serve another image's function at the same address, and the job
then runs the wrong program end to end and returns cleanly.
`spu_lookup` reports that substitution on stderr as `CROSS-IMAGE dispatch`.

A thread therefore **starts in the image its entry point belongs to**, which
the runtime asks the registry for (`spu_image_of_function`). Nothing in the
lv2 layer knows any image ids.

For code that is streamed into local store at run time -- SPURS job binaries,
codec overlays -- the image is selected by **where the code came from**
rather than by where it landed. A title registers each overlay's source
effective address:

```c
spu_overlay_register_source(0x02023680, /* image */ 2);
```

and the MFC GET path recognises that EA, records the overlay as resident in
that context, and dispatch resolves a missed lookup against it
(`spu_context.resident_ovl`). `spu_overlay_register_sig` does the same by
content signature for a binary whose source EA is not stable.

### What the thread is handed

```
spu_context (heap-allocated per thread, freed at group_destroy)
  ls[256 KB]   the image's segments, deployed from the sys_spu_image:
               COPY segments memcpy'd from their source EA, FILL segments
               memset to their value, INFO not loaded
  pc           the image's entry point
  image_id     the image the registry says owns that entry point
  gpr[3..6]    sys_spu_thread_argument's four u64s, one per register, each
               in the register's PREFERRED DOUBLEWORD (lanes 0 and 1)
  spu_id       the tid from sys_spu_thread_initialize
  spu_group_id the parent group
```

The arguments are **copied when the thread is initialized**, not read when
the group starts. lv2 copies them at initialize, and a title reuses one
guest argument block for every thread in a group, rewriting it between
calls; reading it lazily hands every thread the last thread's values.
`sys_spu_thread_set_argument` re-copies them.

`r1` is deliberately left at zero. An SPU image's own crt sets its stack up
and lv2 does not. (The job helpers in `spu_lifted_job.h` do seed `r1`,
because they enter a lifted function directly rather than starting a
thread -- a different ABI, see below.)

### How the thread ends

The SPU-side `sys_spu_thread_exit` ABI writes the status to `SPU_WrOutMbox`
and then executes `stop 0x102`. The stop code is the selector, not the
status (CBEA p97, `SPU_Status.StopCode`):

| stop code | meaning | where the mailbox value goes |
|---|---|---|
| `0x102` | `THREAD_EXIT` | this thread's exit status |
| `0x101` | `GROUP_EXIT` | the group's exit status, cause `GROUP_EXIT` |
| anything else, or a halt | a fault | thread exits 0, caller is told |

`sys_spu_thread_group_join` collects the worst (most negative) thread status
into the group's, unless a thread reported `GROUP_EXIT` or a terminate set
`TERMINATED`, in which case that cause and its status stand.

### Local store

A thread running lifted code owns its local store inside its `spu_context`,
and `sys_spu_thread_read_ls` / `_write_ls` and `spu_thread_get_local_store`
all reach that same 256 KB -- so the PPU reads what the SPU actually wrote.
A thread on the fallback or interpreter paths gets the separate lazily
allocated buffer described under [Local store](#local-store-1) below.

### Where it lives

| File | What |
|---|---|
| `runtime/spu/spu_lifted_thread.c` | image deploy, argument registers, image selection, run, exit classification |
| `runtime/syscalls/lv2_register.c` | the group state machine, the host threads, the completion events |
| `runtime/spu/spu_channels.c` | the function registry, `spu_indirect_branch`, `spu_run_with_halt` |
| `runtime/spu/tests/test_spu_lifted_start.c` | a two-thread group on a synthetic image, with no guest |

## PPU fallback API

The SPU PPU-fallback registry lets a per-game shim provide a PPU-side
implementation for any SPU job, keyed on the SPU image's entry point, for
images that have no lifted code.

`#include "ps3emu/spu_fallback.h"`

```c
typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

int  spu_register_ppu_fallback(uint32_t entry_point,
                               spu_ppu_fallback_fn handler, void* user);
int  spu_unregister_ppu_fallback(uint32_t entry_point);
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry_point,
                                            void** out_user);

/* Local store access (256 KB per thread, lazily allocated) */
uint8_t* spu_thread_get_local_store(uint32_t tid);
uint32_t spu_thread_local_store_size(void);
```

Handler args:
- `tid` — synthesized SPU thread id from `sys_spu_thread_initialize`
- `args_ea` — guest EA of the args block (set via `sys_spu_thread_set_argument`
  or the args parameter of `sys_spu_thread_initialize`)
- `args_size` — currently always 0 (size isn't part of the syscall API)
- `user` — opaque pointer registered alongside the handler

Return value becomes the SPU thread's exit status. The worst (most
negative) status across all threads in a group becomes the group's
exit status, reported back via `sys_spu_thread_group_join`.

## Lifecycle

Register at startup, before any SPU activity:

```c
static int32_t my_decompress_fallback(uint32_t tid, uint32_t args_ea,
                                      uint32_t args_size, void* user)
{
    /* args_ea points at a guest struct the SPU job would have processed.
     * Decode it via vm_read*; do the work on the host; write results
     * back via vm_write*. */
    uint32_t src_ea  = vm_read32(args_ea + 0);
    uint32_t dst_ea  = vm_read32(args_ea + 4);
    uint32_t src_len = vm_read32(args_ea + 8);
    /* ... read src bytes from vm_base + src_ea, decompress on host,
     * write to vm_base + dst_ea ... */
    return 0;  /* CELL_OK */
}

static void register_my_spu_fallbacks(void)
{
    /* Entry point comes from sys_spu_image_open: it parses the ELF and
     * writes the entry to image+4. The "[SPU] image_open" log line
     * shows it; you'll typically read it once with an instrumented run
     * and then hard-code it. */
    spu_register_ppu_fallback(0x000028F0, my_decompress_fallback, NULL);
}
```

Find the entry point via the `[SPU] image_open` log:

```
[SPU] image_open img=0x10001234 path='/dev_flash/sys/spu/decompress.elf' entry=0x000028F0
```

## Execution model

- Synchronous registration; not thread-safe. Call all
  `spu_register_ppu_fallback()` once at startup.
- A fallback is only consulted for an image with **no lifted code**. If
  the registry has a function at the entry point, that runs instead; a
  fallback registered for the same entry point is not reached.
- Asynchronous execution. `sys_spu_thread_group_start` spawns one host
  thread per registered fallback (Win32 `CreateThread`, POSIX
  `pthread_create`), with the same reserved stack a lifted thread gets.
  Threads with neither a fallback nor lifted code complete instantly with
  status 0.
- `sys_spu_thread_group_join` blocks on each running thread's completion
  event, then collects the worst exit status into the group state -- unless
  a thread reported `GROUP_EXIT`, or a terminate set `TERMINATED`, in
  which case that cause and its status stand.
- `sys_spu_thread_get_exit_status` returns CELL_ESTAT (0x80010003) if the
  thread is still in flight — match Sony's documented behaviour.

## Local store

Each SPU thread has a virtual 256 KB local store, allocated lazily on
first `sys_spu_thread_write_ls` / `_read_ls` syscall (or on first
`spu_thread_get_local_store` call). PPU code uses the syscalls; the
fallback handler reaches the same buffer via `spu_thread_get_local_store(tid)`.

A thread running lifted code is the exception: its local store is the one
inside its `spu_context`, because that is what the SPU code reads and
writes, and all three of those entry points hand that back instead. The
buffer is freed at `sys_spu_thread_group_destroy` either way.

Typical pattern:

```c
/* PPU side, before group_start */
sys_spu_thread_write_ls(tid, /*offset*/ 0x100, /*value*/ src_ea, /*type*/ 4);
sys_spu_thread_write_ls(tid, /*offset*/ 0x104, /*value*/ dst_ea, /*type*/ 4);

/* Fallback handler */
static int32_t my_decompress_fallback(uint32_t tid, uint32_t args_ea,
                                      uint32_t args_size, void* user)
{
    uint8_t* ls = spu_thread_get_local_store(tid);
    uint32_t src_ea = (ls[0x100] << 24) | (ls[0x101] << 16) |
                      (ls[0x102] <<  8) |  ls[0x103];
    uint32_t dst_ea = (ls[0x104] << 24) | (ls[0x105] << 16) |
                      (ls[0x106] <<  8) |  ls[0x107];
    /* ... do the work, write completion flag back to LS ... */
    ls[0x200] = 1;
    return 0;
}

/* PPU side, after group_join */
uint64_t done = 0;
sys_spu_thread_read_ls(tid, /*offset*/ 0x200, &done, /*type*/ 1);
```

The buffer is freed when `sys_spu_thread_group_destroy` runs.

## Caveats

- The fallback runs on a host thread, not in the guest VM. It cannot
  call recompiled guest functions or take guest locks. It can read/write
  guest memory freely via `vm_read*` / `vm_write*` helpers.
- Be deterministic about output bytes — games may hash/checksum results.
- If you need to coordinate with PPU code that's running concurrently,
  use the existing host-side sync primitives (mutexes, atomics). Do
  *not* use the guest's lwmutex APIs from a fallback.
- The args_size parameter is currently always 0. If your job needs to
  know the descriptor size, encode it in the descriptor itself.

## Lock-line coherence

A thread running lifted code shares 128-byte lines with the PPU and with the
other SPUs, and `GETLLAR` / `PUTLLC` is how the SPURS kernel arbitrates them. A
store by any other processor into a reserved line has to break that reservation
and raise `SPU_EVENT_LR` on the SPU holding it, or the SPU sleeps through the
write and a pending `PUTLLC` commits against a line that has already moved.

Both writers do it. The PPU store paths consult the reserved-line bitmap and
notify under the lock-line lock. On the SPU side a committing `PUTLLC`, a
`PUTLLUC` and a plain DMA `PUT` into a reserved line notify the same way, from
inside the same lock: five SPURS kernel threads share one management line, so
another SPU is usually the processor doing the writing. A fallback needs
nothing for any of this, because its guest writes go through `vm_write*` and
those are already the coherent paths.

Three parts of the reference implementation are deliberately left out: the
per-line write generation counter, the `GETLLAR` path that serves a cached copy
of the line instead of re-reading it, and the backoff ladder on a repeated
same-line `GETLLAR`. They are one mechanism in three parts, and only the
backoff motivates the other two. `GETLLAR` here always re-reads the line under
the lock, which is what the first macOS boot measured SPURS dispatch working
on, so it already observes a write that landed while no reservation was live
and there is no dropped edge for a generation counter to recover. If a backoff
is ever added, all three come back together.

Described in full under "Lock-Line Coherence" in `docs/RUNTIME.md`.

## Related

- `runtime/syscalls/lv2_register.c` — SPU group/thread state machine and
  the dispatch site in `sys_spu_thread_group_start_handler`.
- `include/ps3emu/spu_fallback.h` — public header.
- `runtime/syscalls/spu_fallback.c` — registry implementation.
- `runtime/spu/spu_lifted_thread.{c,h}` -- starting a thread on lifted code.
- `runtime/spu/spu_lifted_job.h` -- a different ABI for a different job:
  running one lifted function as a SPURS *task* or job body, which takes a
  descriptor EA in `r3` and seeds `r1`, rather than starting a thread.
- `docs/SPU_LIFTER.md` -- how the lifted C is generated, and what
  `spu_register_function` is called with.
