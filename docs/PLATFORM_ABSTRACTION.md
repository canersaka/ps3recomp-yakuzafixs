# Platform Abstraction Layer

How ps3recomp handles cross-platform compatibility between Windows and POSIX (Linux/macOS) systems.

---

## Table of Contents

1. [Design Philosophy](#design-philosophy)
2. [Threading](#threading)
3. [Synchronization Primitives](#synchronization-primitives)
4. [Networking](#networking)
5. [Timers and Clocks](#timers-and-clocks)
6. [Memory Management](#memory-management)
7. [Audio](#audio)
8. [Input](#input)
9. [Cryptographic RNG](#cryptographic-rng)
10. [Filesystem Differences](#filesystem-differences)
11. [Fiber / Coroutine Support](#fiber--coroutine-support)
12. [The Host Shim (`runtime/platform/`)](#the-host-shim-runtimeplatform)
13. [Apple Silicon Notes](#apple-silicon-notes)

---

## Design Philosophy

ps3recomp uses **inline platform switches** (`#ifdef _WIN32`) rather than a separate abstraction layer. This keeps the code direct and avoids unnecessary indirection.

The pattern is:

```c
int32_t some_operation(void)
{
#ifdef _WIN32
    // Windows implementation using Win32 API
    HANDLE h = CreateSomething(...);
    if (!h) return CELL_ENOMEM;
#else
    // POSIX implementation using pthreads/mmap/etc.
    int rc = posix_function(...);
    if (rc != 0) return CELL_ENOMEM;
#endif
    return CELL_OK;
}
```

This approach is used consistently across all runtime and HLE module code.

---

## Threading

### Thread Creation

| PS3 API | Windows | POSIX |
|---------|---------|-------|
| `sys_ppu_thread_create` | `CreateThread()` | `pthread_create()` |
| `sys_ppu_thread_exit` | `ExitThread()` | `pthread_exit()` |
| `sys_ppu_thread_join` | `WaitForSingleObject()` | `pthread_join()` |
| `sys_ppu_thread_detach` | `CloseHandle()` | `pthread_detach()` |
| `sys_ppu_thread_yield` | `SwitchToThread()` | `sched_yield()` |
| `sys_ppu_thread_rename` | `SetThreadDescription()` | `pthread_setname_np()` |

### Thread Local Storage

Thread-local `ppu_context` pointers are stored using:
- Windows: `__declspec(thread)` or `TlsAlloc()` / `TlsGetValue()`
- POSIX: `__thread` or `pthread_key_create()` / `pthread_getspecific()`

### Thread Priority

PS3 priorities (0 = highest, 3071 = lowest) are mapped approximately to host priorities:

**Windows:**
```c
if (priority < 500)       SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
else if (priority < 1000) SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
else if (priority < 2000) SetThreadPriority(h, THREAD_PRIORITY_NORMAL);
else if (priority < 2500) SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
else                      SetThreadPriority(h, THREAD_PRIORITY_LOWEST);
```

**POSIX:**
```c
struct sched_param param;
param.sched_priority = sched_get_priority_max(SCHED_OTHER)
                     - (priority * range / 3071);
pthread_setschedparam(thread, SCHED_OTHER, &param);
```

---

## Synchronization Primitives

### Mutexes

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `CRITICAL_SECTION` | `pthread_mutex_t` |
| **Init** | `InitializeCriticalSection()` | `pthread_mutex_init()` |
| **Lock** | `EnterCriticalSection()` | `pthread_mutex_lock()` |
| **Try Lock** | `TryEnterCriticalSection()` | `pthread_mutex_trylock()` |
| **Unlock** | `LeaveCriticalSection()` | `pthread_mutex_unlock()` |
| **Destroy** | `DeleteCriticalSection()` | `pthread_mutex_destroy()` |
| **Recursive** | Always recursive by default | Set `PTHREAD_MUTEX_RECURSIVE` attr |

**Why `CRITICAL_SECTION` over `CreateMutex`?**
- `CRITICAL_SECTION` is much faster (user-space fast path, no kernel transition)
- Sufficient for intra-process synchronization (which is all we need)
- No named-object overhead

### Condition Variables

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `CONDITION_VARIABLE` | `pthread_cond_t` |
| **Init** | `InitializeConditionVariable()` | `pthread_cond_init()` |
| **Wait** | `SleepConditionVariableCS()` | `pthread_cond_wait()` |
| **Timed Wait** | `SleepConditionVariableCS(timeout)` | `pthread_cond_timedwait()` |
| **Signal** | `WakeConditionVariable()` | `pthread_cond_signal()` |
| **Broadcast** | `WakeAllConditionVariable()` | `pthread_cond_broadcast()` |
| **Destroy** | No-op (statically allocated) | `pthread_cond_destroy()` |

### Semaphores

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `HANDLE` (Win32 Semaphore) | `sem_t` |
| **Create** | `CreateSemaphoreW(NULL, init, max, NULL)` | `sem_init(&sem, 0, init)` |
| **Wait** | `WaitForSingleObject(h, INFINITE)` | `sem_wait(&sem)` |
| **Try Wait** | `WaitForSingleObject(h, 0)` | `sem_trywait(&sem)` |
| **Timed Wait** | `WaitForSingleObject(h, ms)` | `sem_timedwait(&sem, &ts)` |
| **Post** | `ReleaseSemaphore(h, count, NULL)` | `sem_post(&sem)` × count |
| **Destroy** | `CloseHandle(h)` | `sem_destroy(&sem)` |

### Read-Write Locks

| Feature | Windows | POSIX |
|---------|---------|-------|
| **Type** | `SRWLOCK` | `pthread_rwlock_t` |
| **Init** | `InitializeSRWLock()` | `pthread_rwlock_init()` |
| **Read Lock** | `AcquireSRWLockShared()` | `pthread_rwlock_rdlock()` |
| **Write Lock** | `AcquireSRWLockExclusive()` | `pthread_rwlock_wrlock()` |
| **Try Read** | `TryAcquireSRWLockShared()` | `pthread_rwlock_tryrdlock()` |
| **Try Write** | `TryAcquireSRWLockExclusive()` | `pthread_rwlock_trywrlock()` |
| **Read Unlock** | `ReleaseSRWLockShared()` | `pthread_rwlock_unlock()` |
| **Write Unlock** | `ReleaseSRWLockExclusive()` | `pthread_rwlock_unlock()` |

---

## Networking

### Socket API

| Operation | Windows (Winsock2) | POSIX |
|-----------|-------------------|-------|
| **Initialize** | `WSAStartup(MAKEWORD(2,2), &wsa)` | No-op |
| **Shutdown** | `WSACleanup()` | No-op |
| **Socket** | `socket()` | `socket()` |
| **Close** | `closesocket(fd)` | `close(fd)` |
| **Error** | `WSAGetLastError()` | `errno` |
| **Non-blocking** | `ioctlsocket(fd, FIONBIO, &mode)` | `fcntl(fd, F_SETFL, O_NONBLOCK)` |
| **Poll** | `WSAPoll()` | `poll()` |

### Error Code Translation

Winsock errors are translated to PS3 errno values:

```c
static int winsock_to_ps3_errno(int wsa_err)
{
    switch (wsa_err) {
    case WSAEWOULDBLOCK:   return SYS_NET_EWOULDBLOCK;
    case WSAECONNREFUSED:  return SYS_NET_ECONNREFUSED;
    case WSAETIMEDOUT:     return SYS_NET_ETIMEDOUT;
    case WSAEINPROGRESS:   return SYS_NET_EINPROGRESS;
    case WSAEALREADY:      return SYS_NET_EALREADY;
    case WSAECONNRESET:    return SYS_NET_ECONNRESET;
    // ... more mappings
    default:               return SYS_NET_EINVAL;
    }
}
```

### DNS Resolution

Both platforms use `getaddrinfo()` for DNS resolution (Winsock2 and POSIX both support it). The main difference is initialization — Winsock2 requires `WSAStartup()` before any network calls.

### Socket Address Handling

PS3 uses big-endian `sockaddr_in`. The conversion:

```c
// PS3 sockaddr → host sockaddr
struct sockaddr_in host_addr;
host_addr.sin_family = AF_INET;
host_addr.sin_port = ps3_addr->sin_port;      // Already in network byte order
host_addr.sin_addr.s_addr = ps3_addr->sin_addr; // Already in network byte order
```

---

## Timers and Clocks

### High-Resolution Timer

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Get current time** | `QueryPerformanceCounter(&li)` | `clock_gettime(CLOCK_MONOTONIC, &ts)` |
| **Get frequency** | `QueryPerformanceFrequency(&li)` | Fixed: 1,000,000,000 (nanoseconds) |
| **Convert to PS3 ticks** | `ticks = li.QuadPart * 79800000 / freq` | `ticks = ts.tv_sec * 79800000 + ts.tv_nsec * 79800000 / 1000000000` |

### Wall Clock Time

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Current time** | `GetSystemTimeAsFileTime()` → convert | `clock_gettime(CLOCK_REALTIME, &ts)` |
| **Local time** | `GetLocalTime(&st)` | `localtime_r(&time, &tm)` |
| **Timezone** | `GetTimeZoneInformation(&tz)` | `tm.tm_gmtoff` |

### Sleep

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Microsecond sleep** | `Sleep(usec / 1000)` (ms granularity) | `usleep(usec)` |
| **Second sleep** | `Sleep(sec * 1000)` | `sleep(sec)` |

Note: Windows `Sleep()` has millisecond granularity. For sub-millisecond sleeps, a busy-wait or `timeBeginPeriod(1)` may be needed for accuracy.

---

## Memory Management

### Virtual Memory

| Operation | Windows | POSIX |
|-----------|---------|-------|
| **Reserve** | `VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS)` | `mmap(NULL, size, PROT_NONE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE, -1, 0)` |
| **Commit** | `VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE)` | `mprotect(addr, size, PROT_READ\|PROT_WRITE)` |
| **Protect** | `VirtualProtect(addr, size, prot, &old)` | `mprotect(addr, size, prot)` |
| **Release** | `VirtualFree(base, 0, MEM_RELEASE)` | `munmap(base, size)` |

### Protection Flags

| Desired Access | Windows | POSIX |
|---------------|---------|-------|
| No access | `PAGE_NOACCESS` | `PROT_NONE` |
| Read only | `PAGE_READONLY` | `PROT_READ` |
| Read/Write | `PAGE_READWRITE` | `PROT_READ \| PROT_WRITE` |
| Execute/Read | `PAGE_EXECUTE_READ` | `PROT_EXEC \| PROT_READ` |
| All | `PAGE_EXECUTE_READWRITE` | `PROT_READ \| PROT_WRITE \| PROT_EXEC` |

---

## Audio

### Backends

| Platform | Primary Backend | Fallback |
|----------|----------------|----------|
| Windows | WASAPI | SDL2 |
| Linux | SDL2 | PulseAudio (potential future) |
| macOS | SDL2 | CoreAudio (potential future) |

### WASAPI (Windows)

```c
// Exclusive mode for low latency
IAudioClient* client;
IMMDevice* device;
// CoCreateInstance → IMMDeviceEnumerator → GetDefaultAudioEndpoint
// client->Initialize(AUDCLK_SHARED, 0, bufferDuration, 0, &fmt, NULL)
// client->GetService(IID_IAudioRenderClient, &renderClient)
// renderClient->GetBuffer(frames, &data)
// ... write PCM samples ...
// renderClient->ReleaseBuffer(frames, 0)
```

### SDL2 (Cross-platform)

```c
SDL_AudioSpec spec = {
    .freq = 48000,
    .format = AUDIO_F32SYS,
    .channels = 2,
    .samples = 256,
    .callback = audio_callback,
    .userdata = &audio_state
};
SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &spec, &obtained, 0);
SDL_PauseAudioDevice(dev, 0);  // Start playback
```

---

## Input

### Gamepad Backends

| Platform | Primary Backend | Notes |
|----------|----------------|-------|
| Windows | XInput | Native Xbox controller support; DS3 via DS3 drivers |
| Linux/macOS | SDL2 GameController | Supports DualShock 3/4, Xbox, Switch Pro, etc. |

### XInput (Windows)

```c
XINPUT_STATE state;
if (XInputGetState(port, &state) == ERROR_SUCCESS) {
    // Map to PS3 pad data
    pad_data->button[0] |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A)
                           ? CELL_PAD_CTRL_CROSS : 0;
    pad_data->button[4] = (state.Gamepad.sThumbLX + 32768) >> 8;  // Left X
    // ...
}
```

### SDL2 GameController

```c
SDL_GameController* gc = SDL_GameControllerOpen(index);
if (gc) {
    int16_t lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    bool cross = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A);
    // Map to PS3 pad data
}
```

---

## Cryptographic RNG

| Platform | API | Notes |
|----------|-----|-------|
| Windows | `BCryptGenRandom(NULL, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` | Requires `bcrypt.lib` |
| POSIX | `read(open("/dev/urandom", O_RDONLY), buf, len)` | Always available on Linux/macOS |

Used by cellSsl for certificate-related operations and random number generation.

---

## Filesystem Differences

### Path Separators

PS3 uses `/` (forward slash). Windows uses `\` (backslash) but the Win32 API accepts both. ps3recomp uses `/` internally and lets the OS handle it.

### File Attributes

PS3's `cellFsStat` returns big-endian metadata. The host-to-PS3 conversion:

```c
// Host stat → PS3 CellFsStat
ps3_stat->st_mode   = host_to_be32(host_stat.st_mode);
ps3_stat->st_size   = host_to_be64(host_stat.st_size);
ps3_stat->st_atime  = host_to_be64(host_stat.st_atime);
ps3_stat->st_mtime  = host_to_be64(host_stat.st_mtime);
ps3_stat->st_ctime  = host_to_be64(host_stat.st_ctime);
```

### File Open Flags

| PS3 Flag | Value | Windows | POSIX |
|----------|-------|---------|-------|
| `CELL_FS_O_RDONLY` | 0 | `GENERIC_READ` | `O_RDONLY` |
| `CELL_FS_O_WRONLY` | 1 | `GENERIC_WRITE` | `O_WRONLY` |
| `CELL_FS_O_RDWR` | 2 | `GENERIC_READ \| GENERIC_WRITE` | `O_RDWR` |
| `CELL_FS_O_CREAT` | 0x100 | `CREATE_ALWAYS` | `O_CREAT` |
| `CELL_FS_O_TRUNC` | 0x200 | `TRUNCATE_EXISTING` | `O_TRUNC` |
| `CELL_FS_O_APPEND` | 0x400 | `FILE_APPEND_DATA` | `O_APPEND` |

---

## Fiber / Coroutine Support

### Windows Fibers

```c
// Convert current thread to a fiber
ConvertThreadToFiber(NULL);

// Create a new fiber
LPVOID fiber = CreateFiber(stack_size, fiber_proc, param);

// Switch to fiber
SwitchToFiber(fiber);

// Delete fiber
DeleteFiber(fiber);
```

### POSIX ucontext

```c
ucontext_t ctx;
getcontext(&ctx);
ctx.uc_stack.ss_sp = stack_memory;
ctx.uc_stack.ss_size = stack_size;
ctx.uc_link = &return_context;
makecontext(&ctx, fiber_proc, 1, param);

// Switch to fiber
swapcontext(&current, &ctx);
```

**Note:** `ucontext` is deprecated on macOS but still works. For production macOS builds, consider using `setjmp`/`longjmp` with manual stack management, or platform-specific alternatives.

---

## The Host Shim (`runtime/platform/`)

The inline `#ifdef _WIN32` pattern above is right for HLE modules, where each
branch is a few lines. It is the wrong tool for code written *entirely* against
Win32 -- the PPU boot scaffold, the sync stress suite, and a game's own host
code, where the call sites number in the hundreds. For those, the Win32 names
are supplied on POSIX instead, and the call sites stand:

| Header | Provides | On Windows |
|---|---|---|
| `win32_compat.h` + `win32_compat.c` | Types (`DWORD`, `LONG`, `HANDLE`, `LARGE_INTEGER`, `CONTEXT`, `EXCEPTION_POINTERS`...), the Interlocked family (32/64-bit, pointer, both spellings), `ReadAcquire`/`WriteRelease`, barriers, `SRWLOCK` (both modes), `CONDITION_VARIABLE`, `CRITICAL_SECTION`, events, semaphores, joinable threads, waitable timers, `WaitForSingleObject`/`WaitForMultipleObjects`, `WaitOnAddress`, `Sleep`/QPC/`GetTickCount64`, thread priority/affinity/description, thread control (`SuspendThread`/`ResumeThread`/`GetThreadContext`/`SetThreadContext`/`OpenThread`/`DuplicateHandle`/`GetThreadTimes`/`CreateToolhelp32Snapshot`), `VirtualAlloc`/`VirtualProtect`/`VirtualFree`/`VirtualQuery`, `IsBadReadPtr`/`IsBadWritePtr`, `AddVectoredExceptionHandler`/`RemoveVectoredExceptionHandler`/`SetUnhandledExceptionFilter`, `GetSystemInfo`, `GetLastError` | passthrough to `<windows.h>` |
| `msvc_compat.h` | The MSVC CRT and intrinsic dialect: `_snprintf`, `fopen_s`, `strcpy_s`, `_stricmp`, `_fseeki64`, `_mkdir`, `_stat`, `_byteswap_*`, `_BitScanForward`, `__popcnt`, `_umul128`, `_ReturnAddress`, `__debugbreak`, `__declspec(thread/noinline/align)`, `__forceinline` | passthrough to `<intrin.h>` and the CRT |
| `win32_backtrace.h` | `RtlCaptureStackBackTrace`, `GetModuleHandleA/ExA`, `GetModuleFileNameA`, and the dbghelp names a crash report uses (`SymInitialize`, `SymFromAddr`, `SymCleanup`), over `backtrace(3)` and `dladdr(3)` | passthrough |
| `win32_dirent.h` | `<dirent.h>` for MSVC | a `FindFirstFile` implementation |
| `darwin_compat.h` | `pthread_mutex_timedlock`, `sem_timedwait` on Darwin | n/a |
| `posix_sem.h` | `ps3_sem_t`: a counting semaphore with a real timed wait, for hosts where `sem_init` is a stub (Darwin) | n/a |

Rules the shim keeps, and code using it must keep too:

- **`LONG` is 32 bits** on every host, as it is on Windows. The LP64 `long` is
  not an acceptable stand-in: `InterlockedExchange((volatile LONG*)&u32, ...)`
  through a 64-bit `LONG` is an 8-byte read-modify-write on a 4-byte object.
  Declare interlocked targets `LONG`/`LONG64`, never `long`.
- **`SRWLOCK` and `CONDITION_VARIABLE` stay pointer-sized** so they fit the
  `void*` slots the SPU context stores them in. The real pthread object is
  heap-allocated on first use; zero initialisation remains valid.
- **Condition variables work with any lock** (SRW in either mode, or a
  critical section) through a generation counter, so they are not tied to a
  pthread mutex.
- **Waitable handles share one lock**, which is what makes
  `WaitForMultipleObjects(bWaitAll)` atomic. The runtime's hot paths use
  pthreads directly in their `#else` branches; the shim carries the coarse,
  Win32-shaped traffic it was written for.
- **`WakeByAddressSingle` broadcasts.** Addresses hash to buckets, so a single
  signal could wake the wrong waiter; callers already re-check the value, as
  the Win32 contract requires.
- **A suspended thread still holds what it held.** `SuspendThread` stops a
  running thread for real, and inherits the Win32 hazard with it: a thread
  frozen inside `malloc` still owns `malloc`'s lock, so the thread that
  suspended it must not allocate before it resumes it. Suspending the CALLING
  thread is refused (`(DWORD)-1`) rather than deadlocking the shim, which is
  the one place this deliberately differs from Windows.
- **A thread the shim did not create is adopted** into the thread table the
  first time `DuplicateHandle`, `OpenThread` or a snapshot names it, and the
  record is released by a TLS destructor at that thread's exit. Nothing tells
  the shim when a foreign thread ends before that, so an adopted record has no
  exit code and never becomes signaled: waiting on one waits forever.
- **`VirtualProtect` cannot report the old protection**; it reports
  `PAGE_READWRITE`, so a restore errs towards accessible. `VirtualQuery` does
  report the current one, so a caller that wants the real value asks for it.
- **`VirtualQuery`'s `State` is the shim's own model** of reserve and commit,
  because that is what it built the mapping out of: `PROT_NONE` is
  `MEM_RESERVE`, anything accessible is `MEM_COMMIT`, nothing mapped is
  `MEM_FREE`. A page the process made `PROT_NONE` for its own reasons reads as
  reserved, which is the same conflation Win32 makes with `PAGE_NOACCESS` on a
  committed page.
- **The exception dispatcher takes no lock and chains.** The handler list is
  read through atomic loads, so a fault on a thread holding the shim's lock is
  not a deadlock; and after the handlers and the unhandled filter, the signal
  goes to whatever `sigaction` was installed before the shim. That is what
  makes it compose with `runtime/ppu/ppu_loader.cpp`, which installs its own
  `SIGSEGV` handler and chains the same way, in either installation order.

### Where each of the game-runner calls lands

`docs/MACOS_PORT.md` names the point a Win32 runner used to stop compiling
against the shim. These are the calls that were on that list.

| Call | macOS | Linux | Not covered |
|---|---|---|---|
| `SuspendThread`, `ResumeThread` | `thread_suspend` / `thread_resume` on the thread's Mach port | `SIGRTMIN+4` parks the thread inside its own handler until the resume | suspending the calling thread |
| `GetThreadContext`, `SetThreadContext` | `thread_get_state` / `thread_set_state`, `ARM_THREAD_STATE64` or `x86_THREAD_STATE64` | the registers the park handler saved, written back into the signal frame as it returns | floating point, vector and segment state; the x86 debug registers, so no hardware watchpoints |
| `OpenThread`, `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)`, `Thread32First`/`Next` | the shim's thread table | same | threads the C library or a framework created; any other process |
| `DuplicateHandle` | another reference to the same object; the current-thread pseudo handle adopts the caller | same | cross-process duplication |
| `GetThreadTimes` | the shim's creation and exit stamps; `thread_info THREAD_BASIC_INFO` splits kernel from user | `pthread_getcpuclockid`, which does not split: the total is reported as user time | a thread's true creation time -- the stamp is when the shim learned of it |
| `VirtualQuery` | `mach_vm_region`, with the shim's reservation table for `AllocationBase` | `/proc/self/maps`, the same | `Type` is always `MEM_PRIVATE` |
| `IsBadReadPtr`, `IsBadWritePtr` | one Mach trap per region walked | one `/proc/self/maps` read per region walked: not for a hot loop | the Win32 race is unchanged -- the answer describes the address space at the moment it was taken |
| `AddVectoredExceptionHandler`, `SetUnhandledExceptionFilter` | `sigaction` on `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`; the write bit from the ESR's `WnR` | the same, with the ESR read out of the signal frame's extension records on arm64 and the page-fault error code on x86-64 | `__try`/`__except`, `RaiseException`, and a stack overflow unless the process installed its own `sigaltstack` |
| `SymInitialize`, `SymFromAddr`, `SymCleanup`, `GetModuleFileNameA` | `dladdr`; an unstripped image carries local symbols, so a file-static function usually resolves | `dladdr` sees the dynamic symbol table only: an executable's own functions need `-rdynamic`, a `static` one never resolves | line numbers, inlined frames, a symbol's extent, a separate symbol file |

`runtime/platform/tests/test_win32_compat.c` is the contract: return
conventions, widths, one waiter per `SetEvent`, both wait modes, timers,
`VirtualAlloc` reserve/commit/protect/release, a spinning thread frozen and
released, `VirtualQuery` over a reservation, a commit, a stack and a gap, a
vectored handler that repairs a fault and continues, and one that declines and
falls through to the unhandled filter. It runs in the Linux and macOS
workflows, and `tests/sync_stress` runs on the same shim on every platform.

---

## Apple Silicon Notes

What differs on an arm64 Mac from the x86-64 Windows machines the runtime was
validated on, and where each is handled:

| Difference | Where |
|---|---|
| **Weak memory model.** x86 is TSO; arm64 reorders freely. Lifted guest code carries its own fences (`sync`/`lwsync`/`eieio`/`isync` and the SPU `sync`/`dsync` lift to C11 fences), so the audit target is host runtime code that used `volatile` or a plain store to publish state. Every "data stores, then flag store" needs a release/acquire pair; the shim's Interlocked ops are sequentially consistent and `ReadAcquire`/`WriteRelease` are there for the flag case. | `runtime/platform/win32_compat.h`, per-module |
| **16 KB pages.** `mprotect` on a 4 KB-aligned address fails with `EINVAL`, and a "4 KB" guard covers 16 KB. `VM_PAGE_SIZE` stays the guest's 4 KB; commit, protect and the stack guard use `vm_host_page_size()`. | `runtime/memory/vm.h` |
| **No unnamed semaphores.** `sem_init` returns `ENOSYS`. | `runtime/platform/posix_sem.h` |
| **A protection fault is `SIGBUS`, not `SIGSEGV`.** Darwin routes an access to a page that is mapped but inaccessible -- a `PROT_NONE` reservation, most of all -- to `SIGBUS`, and only an address with nothing mapped at it to `SIGSEGV`. So a handler installed for one signal and not the other misses half the faults. Worse, `si_code` there is 1, the value that spells `BUS_ADRALN`, whether the access was misaligned or merely forbidden; the direction and the real cause come from the trap frame's ESR instead. | `runtime/platform/win32_compat.c` |
| **No thread affinity.** `SetThreadAffinityMask` is a no-op; thread priority maps to QoS classes (`USER_INTERACTIVE` for above-normal and up), which is what keeps a thread on the P-cores. | `runtime/platform/win32_compat.c` |
| **FMA in the base ISA.** Clang contracts `a*b+c` into a fused multiply-add by default on arm64; MSVC never does and x86-64 without `-mfma` cannot. The library builds with `-ffp-contract=off` so results match the validated x86 builds and a PPC `fmadd` is fused only where the lifter spells one -- and a port's own build of the lifted code must set the same flag. | `CMakeLists.txt` |
| **`ucontext` is deprecated** and hidden unless `_XOPEN_SOURCE` is defined first. | `libs/spurs/cellFiber.c` |
| **Darwin names only the calling thread** (`pthread_setname_np(name)`), and `pthread_threadid_np` is the kernel tid. | `runtime/platform/win32_compat.c` |
| **x86 intrinsics.** Spin hints are `pause` on x86 and `yield` on arm64; SSE paths in `stb_image` are guarded by its own `STBI_SSE2` detection. | `runtime/spu/spu_lockstep.c`, `win32_compat.h` |

An x86-64 build of the same tree under Rosetta 2 runs with TSO and 4 KB pages,
so it separates an OS-port bug from an architecture bug when an arm64 hang
needs classifying. Set it up for that, not as the mainline.
