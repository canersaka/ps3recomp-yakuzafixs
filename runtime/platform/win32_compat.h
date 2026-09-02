/*
 * ps3recomp - Win32 sync/timing/type compatibility shim for POSIX hosts
 *
 * A good deal of this tree was written directly against Win32: the
 * synchronisation and timing API (SRWLOCK, CONDITION_VARIABLE, QPC, events),
 * the interlocked intrinsics behind the PPU reservation spinlock, and the
 * scalar typedefs (DWORD, ULONGLONG, SIZE_T, ...) those call sites use. This
 * header supplies all of it on POSIX so the call sites compile unchanged. On
 * Windows it is a no-op passthrough to <windows.h>.
 *
 * Usable from C AND C++. That is deliberate: the runtime library is C but the
 * PPU boot scaffold is C++, and both need these names. Nothing in here may use
 * a C-only construct -- see the note on ps3_lazy_obj below for the one that
 * did, and how far it got before anyone noticed.
 *
 * Two kinds of definition live here:
 *
 *   - static inline functions with no shared state (types, interlocked ops,
 *     SRW locks, condition variables, critical sections, timing). These need
 *     nothing linked.
 *   - functions that need state shared across translation units (waitable
 *     handles -- events, semaphores, threads, timers -- WaitOnAddress, and the
 *     VirtualAlloc region table). Those are declared here and implemented in
 *     win32_compat.c, which the runtime library compiles on every POSIX host.
 *     A standalone tool that calls one of them links that file too.
 *
 * IMPORTANT -- storage size. Win32 SRWLOCK and CONDITION_VARIABLE are exactly
 * pointer-sized and zero-initialise validly, so several structs (notably
 * spu_context::ch_wait_lock / ch_wait_cv) store them inline in a `void*` slot.
 * pthread_rwlock_t (200 B on Darwin) and pthread_cond_t (48 B) do NOT fit
 * there; casting the slot and writing a pthread object through it would
 * overflow into adjacent members. So on POSIX these map to `void*` and the
 * real object is heap-allocated on first use and published into the slot with
 * an atomic CAS. NULL still means "uninitialised", so existing memset-based
 * zero-init remains correct.
 *
 * IMPORTANT -- LONG is 32 bits here, as on Windows. MSVC's `long` is 32-bit and
 * every Win32 declaration of LONG, ULONG and the interlocked targets means
 * exactly that width. A POSIX LP64 `long` is 64 bits, and mapping LONG onto it
 * makes `InterlockedExchange((volatile LONG*)&some_u32, ...)` an 8-byte RMW on
 * a 4-byte object -- silent corruption of whatever sits next to it, on the
 * platform where it is hardest to find. So LONG is int32_t, and a call site
 * that spelt its target `volatile long` fails to compile instead: change it to
 * `volatile LONG`, which is what it always meant.
 */
#ifndef PS3RECOMP_WIN32_COMPAT_H
#define PS3RECOMP_WIN32_COMPAT_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else

#include <pthread.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <wchar.h>
#include <sys/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Scalar types and constants
 * -----------------------------------------------------------------------*/
#ifndef FALSE
#  define FALSE 0
#endif
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef INFINITE
#  define INFINITE 0xFFFFFFFFu
#endif

#ifndef WINAPI
#  define WINAPI
#endif
#ifndef CALLBACK
#  define CALLBACK
#endif
#ifndef APIENTRY
#  define APIENTRY
#endif

typedef int                BOOL;
typedef unsigned char      BYTE;
typedef unsigned char      UCHAR;
typedef char               CHAR;
typedef unsigned short     WORD;
#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short     USHORT;
#endif
typedef short              SHORT;
typedef uint32_t           DWORD;
typedef uint32_t           ULONG;
typedef uint32_t           UINT;
typedef int32_t            LONG;      /* 32-bit, as on Windows -- see the header comment */
typedef int32_t            INT;
typedef int64_t            LONGLONG;
typedef int64_t            LONG64;
typedef uint64_t           ULONGLONG;
typedef uint64_t           ULONG64;
typedef uint64_t           DWORD64;
typedef uint64_t           DWORDLONG;
typedef size_t             SIZE_T;
typedef size_t             ULONG_PTR;
typedef size_t             DWORD_PTR;
typedef intptr_t           LONG_PTR;
typedef intptr_t           SSIZE_T;
typedef void               VOID;
typedef void*              PVOID;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef void*              HANDLE;
typedef HANDLE*            PHANDLE;
typedef char*              LPSTR;
typedef char*              PSTR;
typedef const char*        LPCSTR;
typedef const char*        PCSTR;
typedef wchar_t*           LPWSTR;
typedef wchar_t*           PWSTR;
typedef const wchar_t*     LPCWSTR;
typedef const wchar_t*     PCWSTR;
typedef unsigned int       ULONG32;
typedef int                LONG32;

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;

typedef struct {
    DWORD     dwPageSize;
    LPVOID    lpMinimumApplicationAddress;
    LPVOID    lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO;

#define INVALID_HANDLE_VALUE   ((HANDLE)(intptr_t)-1)

#ifndef MAX_PATH
#  ifdef PATH_MAX
#    define MAX_PATH PATH_MAX
#  else
#    define MAX_PATH 4096
#  endif
#endif

#define WAIT_OBJECT_0          0u
#define WAIT_ABANDONED         0x80u
#define WAIT_ABANDONED_0       0x80u
#define WAIT_TIMEOUT           258u
#define WAIT_FAILED            0xFFFFFFFFu
#define STILL_ACTIVE           259u

#define ERROR_SUCCESS          0u
#define ERROR_INVALID_HANDLE   6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_TIMEOUT          1460u
#define TIMERR_NOERROR         0u

#define CREATE_SUSPENDED       0x00000004u
#define CREATE_EVENT_MANUAL_RESET 0x00000001u
#define CREATE_EVENT_INITIAL_SET  0x00000002u
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN  0x7FFFFFFF

#define IDLE_PRIORITY_CLASS           0x00000040u
#define BELOW_NORMAL_PRIORITY_CLASS   0x00004000u
#define NORMAL_PRIORITY_CLASS         0x00000020u
#define ABOVE_NORMAL_PRIORITY_CLASS   0x00008000u
#define HIGH_PRIORITY_CLASS           0x00000080u
#define REALTIME_PRIORITY_CLASS       0x00000100u

#define MEM_COMMIT                    0x00001000u
#define MEM_RESERVE                   0x00002000u
#define MEM_DECOMMIT                  0x00004000u
#define MEM_RELEASE                   0x00008000u
#define MEM_RESET                     0x00080000u
#define MEM_TOP_DOWN                  0x00100000u
#define PAGE_NOACCESS                 0x01u
#define PAGE_READONLY                 0x02u
#define PAGE_READWRITE                0x04u
#define PAGE_WRITECOPY                0x08u
#define PAGE_EXECUTE                  0x10u
#define PAGE_EXECUTE_READ             0x20u
#define PAGE_EXECUTE_READWRITE        0x40u
#define PAGE_EXECUTE_WRITECOPY        0x80u
#define PAGE_GUARD                    0x100u
#define PAGE_NOCACHE                  0x200u

/* ---------------------------------------------------------------------------
 * Last error (thread-local, in win32_compat.c)
 * -----------------------------------------------------------------------*/
DWORD GetLastError(void);
void  SetLastError(DWORD err);

/* ---------------------------------------------------------------------------
 * Lazy, race-free materialisation of a pthread object into a void* slot
 *
 * __atomic builtins rather than C11 <stdatomic.h>: _Atomic and
 * atomic_load_explicit are C-only spellings, so the C11 version restricted
 * this whole header to .c files. The PPU boot scaffold is C++, and including
 * it from there failed under GCC (Clang accepts _Atomic in C++ as an
 * extension, which is exactly the kind of difference that hides until the
 * other compiler tries). The builtins mean the same thing in both languages.
 * -----------------------------------------------------------------------*/
static inline void* ps3_lazy_obj(void** slot, size_t sz,
                                 void (*init)(void*))
{
    void* cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur) return cur;
    void* fresh = calloc(1, sz);
    if (!fresh) return NULL;
    init(fresh);
    void* expected = NULL;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, 0 /* strong */,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    free(fresh);                 /* lost the race; use the winner's object */
    return expected;
}

/* Absolute CLOCK_REALTIME deadline `ms` from now, for pthread_cond_timedwait.
 * REALTIME rather than MONOTONIC because Darwin has no
 * pthread_condattr_setclock; a wall-clock step during a wait is the accepted
 * price of one code path on every host. */
static inline struct timespec ps3__deadline_ms(DWORD ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}

/* ---------------------------------------------------------------------------
 * Interlocked operations and barriers
 *
 * Every one is a sequentially consistent RMW through the __atomic builtins,
 * which is what the MSVC intrinsics (full-barrier forms) mean. The return
 * conventions are Win32's: Increment/Decrement/Add return the NEW value,
 * Exchange/ExchangeAdd/And/Or/Xor/CompareExchange return the PREVIOUS one.
 * -----------------------------------------------------------------------*/
static inline LONG InterlockedIncrement(volatile LONG* t)            { return __atomic_add_fetch(t, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG* t)            { return __atomic_sub_fetch(t, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG* t, LONG v)     { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG* t, LONG v)  { return __atomic_fetch_add(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedAdd(volatile LONG* t, LONG v)          { return __atomic_add_fetch(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedAnd(volatile LONG* t, LONG v)          { return __atomic_fetch_and(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedOr(volatile LONG* t, LONG v)           { return __atomic_fetch_or(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedXor(volatile LONG* t, LONG v)          { return __atomic_fetch_xor(t, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG* t, LONG exchange, LONG comparand)
{
    __atomic_compare_exchange_n(t, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;            /* holds the previous value either way */
}

static inline LONG64 InterlockedIncrement64(volatile LONG64* t)            { return __atomic_add_fetch(t, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedDecrement64(volatile LONG64* t)            { return __atomic_sub_fetch(t, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchange64(volatile LONG64* t, LONG64 v)   { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64* t, LONG64 v){ return __atomic_fetch_add(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedAdd64(volatile LONG64* t, LONG64 v)        { return __atomic_add_fetch(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedAnd64(volatile LONG64* t, LONG64 v)        { return __atomic_fetch_and(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedOr64(volatile LONG64* t, LONG64 v)         { return __atomic_fetch_or(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedXor64(volatile LONG64* t, LONG64 v)        { return __atomic_fetch_xor(t, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64* t, LONG64 exchange, LONG64 comparand)
{
    __atomic_compare_exchange_n(t, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

static inline PVOID InterlockedExchangePointer(PVOID volatile* t, PVOID v) { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile* t, PVOID exchange, PVOID comparand)
{
    __atomic_compare_exchange_n(t, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

/* The intrinsic spellings are the same operations. */
#define _InterlockedIncrement          InterlockedIncrement
#define _InterlockedDecrement          InterlockedDecrement
#define _InterlockedExchange           InterlockedExchange
#define _InterlockedExchangeAdd        InterlockedExchangeAdd
#define _InterlockedAnd                InterlockedAnd
#define _InterlockedOr                 InterlockedOr
#define _InterlockedXor                InterlockedXor
#define _InterlockedCompareExchange    InterlockedCompareExchange
#define _InterlockedIncrement64        InterlockedIncrement64
#define _InterlockedDecrement64        InterlockedDecrement64
#define _InterlockedExchange64         InterlockedExchange64
#define _InterlockedExchangeAdd64      InterlockedExchangeAdd64
#define _InterlockedAnd64              InterlockedAnd64
#define _InterlockedOr64               InterlockedOr64
#define _InterlockedXor64              InterlockedXor64
#define _InterlockedCompareExchange64  InterlockedCompareExchange64
#define _InterlockedExchangePointer    InterlockedExchangePointer
#define _InterlockedCompareExchangePointer InterlockedCompareExchangePointer

/* Acquire/release loads and stores (winnt.h), for the code that publishes a
 * flag after its data rather than through an RMW. */
static inline LONG   ReadAcquire(const volatile LONG* p)          { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline LONG   ReadNoFence(const volatile LONG* p)          { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void   WriteRelease(volatile LONG* p, LONG v)       { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void   WriteNoFence(volatile LONG* p, LONG v)       { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline LONG64 ReadAcquire64(const volatile LONG64* p)      { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline LONG64 ReadNoFence64(const volatile LONG64* p)      { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void   WriteRelease64(volatile LONG64* p, LONG64 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void   WriteNoFence64(volatile LONG64* p, LONG64 v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline PVOID  ReadPointerAcquire(PVOID const volatile* p)  { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void   WritePointerRelease(PVOID volatile* p, PVOID v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

/* Full hardware fence vs. compiler-only fence: MSVC's MemoryBarrier is the
 * former, _ReadWriteBarrier the latter. Keep them distinct -- collapsing both
 * to a compiler fence is exactly what x86-TSO lets you get away with and arm64
 * does not. */
static inline void MemoryBarrier(void)     { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void _ReadWriteBarrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void _ReadBarrier(void)      { __asm__ __volatile__("" ::: "memory"); }
static inline void _WriteBarrier(void)     { __asm__ __volatile__("" ::: "memory"); }

/* Spin hint: tells the core this is a wait loop, so it can back off rather
 * than burn the pipeline. Purely advisory -- doing nothing is correct, just
 * slower, which is why the fallback is empty rather than a syscall. */
static inline void YieldProcessor(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* ---------------------------------------------------------------------------
 * SRW locks: pointer-sized slot, pthread_rwlock_t materialised on first use.
 *
 * Both lock modes are supported. The Shared variants were absent while only
 * the Exclusive ones were used in-tree; the game runners this header exists
 * for take reader locks on their residency tables, so the mapping had to grow
 * into a real reader-writer lock rather than a mutex.
 * -----------------------------------------------------------------------*/
typedef void*  SRWLOCK;
typedef SRWLOCK* PSRWLOCK;
#define SRWLOCK_INIT NULL

static inline void ps3__rwl_init(void* p) { pthread_rwlock_init((pthread_rwlock_t*)p, NULL); }
static inline pthread_rwlock_t* ps3_srw(SRWLOCK* l)
{ return (pthread_rwlock_t*)ps3_lazy_obj((void**)l, sizeof(pthread_rwlock_t), ps3__rwl_init); }

static inline void InitializeSRWLock(SRWLOCK* l)          { *l = NULL; }
static inline void AcquireSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_wrlock(ps3_srw(l)); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l)    { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) { return pthread_rwlock_trywrlock(ps3_srw(l)) == 0; }
static inline void AcquireSRWLockShared(SRWLOCK* l)       { pthread_rwlock_rdlock(ps3_srw(l)); }
static inline void ReleaseSRWLockShared(SRWLOCK* l)       { pthread_rwlock_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK* l)    { return pthread_rwlock_tryrdlock(ps3_srw(l)) == 0; }

/* ---------------------------------------------------------------------------
 * Critical sections: a recursive mutex, as on Windows.
 *
 * Not a void* slot: CRITICAL_SECTION is an out-of-line struct on Windows too,
 * always initialised through InitializeCriticalSection, so the real object
 * can live inline here.
 * -----------------------------------------------------------------------*/
typedef struct {
    pthread_mutex_t m;
    int             initialised;
} CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;
typedef CRITICAL_SECTION* PCRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
    cs->initialised = 1;
}
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD spin)
{ (void)spin; InitializeCriticalSection(cs); return TRUE; }
static inline BOOL InitializeCriticalSectionEx(CRITICAL_SECTION* cs, DWORD spin, DWORD flags)
{ (void)spin; (void)flags; InitializeCriticalSection(cs); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs)    { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->m) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
{ if (cs->initialised) { pthread_mutex_destroy(&cs->m); cs->initialised = 0; } }

/* ---------------------------------------------------------------------------
 * Condition variables: pointer-sized slot; the object is a mutex, a condvar
 * and a generation counter.
 *
 * Win32 condition variables sleep against an SRW lock in either mode or a
 * critical section, and pthread_cond_wait only knows mutexes. So the wait does
 * not hand the caller's lock to pthread at all: it records the generation
 * under the object's own mutex, releases the caller's lock, sleeps until the
 * generation moves (or the deadline passes), then re-takes the caller's lock
 * in the mode it was held. Wake bumps the generation under the same mutex.
 * There is no lost wakeup: the caller holds its lock while the generation is
 * read, and a waker must hold that lock to change the predicate, so its bump
 * is ordered after the read. Spurious wakeups are allowed, as on Windows.
 * -----------------------------------------------------------------------*/
typedef void*  CONDITION_VARIABLE;
typedef CONDITION_VARIABLE* PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT NULL

typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned gen; } ps3_condvar;

static inline void ps3__cv_init(void* p)
{
    ps3_condvar* cv = (ps3_condvar*)p;
    pthread_mutex_init(&cv->m, NULL);
    pthread_cond_init(&cv->c, NULL);
    cv->gen = 0;
}
static inline ps3_condvar* ps3_cv(CONDITION_VARIABLE* c)
{ return (ps3_condvar*)ps3_lazy_obj((void**)c, sizeof(ps3_condvar), ps3__cv_init); }

static inline void InitializeConditionVariable(CONDITION_VARIABLE* c) { *c = NULL; }
static inline void WakeConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_signal(&cv->c);
    pthread_mutex_unlock(&cv->m);
}
static inline void WakeAllConditionVariable(CONDITION_VARIABLE* c)
{
    ps3_condvar* cv = ps3_cv(c);
    if (!cv) return;
    pthread_mutex_lock(&cv->m);
    cv->gen++;
    pthread_cond_broadcast(&cv->c);
    pthread_mutex_unlock(&cv->m);
}

/* The wait body, shared by the SRW and CS entry points. `unlock`/`relock`
 * are the caller's lock in the mode it holds it. */
static inline BOOL ps3__cv_sleep(ps3_condvar* cv, DWORD ms,
                                 void (*unlock)(void*), void (*relock)(void*), void* lock)
{
    if (!cv) return FALSE;
    pthread_mutex_lock(&cv->m);
    unsigned g = cv->gen;
    unlock(lock);
    int rc = 0;
    if (ms == INFINITE) {
        while (cv->gen == g && rc == 0) rc = pthread_cond_wait(&cv->c, &cv->m);
    } else {
        struct timespec ts = ps3__deadline_ms(ms);
        while (cv->gen == g && rc == 0) rc = pthread_cond_timedwait(&cv->c, &cv->m, &ts);
    }
    BOOL woke = (cv->gen != g);
    pthread_mutex_unlock(&cv->m);
    relock(lock);
    if (!woke) SetLastError(ERROR_TIMEOUT);
    return woke;
}
static inline void ps3__srw_unlock_x(void* l) { ReleaseSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_lock_x  (void* l) { AcquireSRWLockExclusive((SRWLOCK*)l); }
static inline void ps3__srw_unlock_s(void* l) { ReleaseSRWLockShared((SRWLOCK*)l); }
static inline void ps3__srw_lock_s  (void* l) { AcquireSRWLockShared((SRWLOCK*)l); }
static inline void ps3__cs_unlock   (void* l) { LeaveCriticalSection((CRITICAL_SECTION*)l); }
static inline void ps3__cs_lock     (void* l) { EnterCriticalSection((CRITICAL_SECTION*)l); }

static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l, DWORD ms, ULONG flags)
{
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_s, ps3__srw_lock_s, l);
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__srw_unlock_x, ps3__srw_lock_x, l);
}
static inline BOOL SleepConditionVariableCS(CONDITION_VARIABLE* c, CRITICAL_SECTION* cs, DWORD ms)
{
    return ps3__cv_sleep(ps3_cv(c), ms, ps3__cs_unlock, ps3__cs_lock, cs);
}

/* ---------------------------------------------------------------------------
 * Timing
 * -----------------------------------------------------------------------*/
static inline unsigned long long ps3__mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
static inline ULONGLONG GetTickCount64(void) { return ps3__mono_ns() / 1000000ull; }
static inline DWORD     GetTickCount(void)   { return (DWORD)(ps3__mono_ns() / 1000000ull); }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter  (LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)ps3__mono_ns(); return TRUE; }

/* 100 ns units since 1601-01-01, as GetSystemTimeAsFileTime reports. */
static inline void GetSystemTimeAsFileTime(FILETIME* ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long t = (unsigned long long)ts.tv_sec * 10000000ull
                         + (unsigned long long)ts.tv_nsec / 100ull
                         + 116444736000000000ull;
    ft->dwLowDateTime  = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}
static inline void GetSystemTimePreciseAsFileTime(FILETIME* ft) { GetSystemTimeAsFileTime(ft); }

static inline void Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }      /* Sleep(0) yields, as on Windows */
    struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&d, NULL);
}
static inline DWORD SleepEx(DWORD ms, BOOL alertable) { (void)alertable; Sleep(ms); return 0; }
static inline BOOL  SwitchToThread(void)              { sched_yield(); return TRUE; }
static inline UINT  timeBeginPeriod(UINT period)      { (void)period; return TIMERR_NOERROR; }
static inline UINT  timeEndPeriod(UINT period)        { (void)period; return TIMERR_NOERROR; }

/* A stable, per-thread integer id. Only ever logged or hashed, never handed
 * back to the OS, so any injective-per-thread value will do. Darwin and Linux
 * both offer the real kernel tid, which is what a debugger and the guest-side
 * logs show; elsewhere pthread_self is the only portable handle there is.
 *
 * pthread_threadid_np is Darwin-only. Calling it unguarded built fine under
 * GCC -- an implicit declaration into a static archive, which links nothing --
 * and only failed once Clang rejected it. Hence the CI symbol check. */
static inline DWORD GetCurrentThreadId(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (DWORD)tid;
#elif defined(__linux__)
    /* Not gettid(3): that wrapper only appeared in glibc 2.30. */
    return (DWORD)syscall(SYS_gettid);
#else
    return (DWORD)(uintptr_t)pthread_self();
#endif
}
static inline DWORD  GetCurrentProcessId(void) { return (DWORD)getpid(); }
static inline HANDLE GetCurrentProcess(void)   { return (HANDLE)(intptr_t)-1; }   /* pseudo-handle */
static inline HANDLE GetCurrentThread(void)    { return (HANDLE)(intptr_t)-2; }   /* pseudo-handle */

/* ---------------------------------------------------------------------------
 * Waitable handles: events, semaphores, threads, waitable timers
 *
 * Implemented in win32_compat.c. Every object shares one lock, which is what
 * makes WaitForMultipleObjects(bWaitAll) atomic across objects the way Win32
 * defines it; per-object condition variables keep single waits targeted. The
 * runtime's own hot paths use pthreads directly in their POSIX branches, so
 * the shared lock only carries the coarse, Win32-shaped traffic these were
 * written for.
 *
 * Threads: the handle is joinable (WaitForSingleObject blocks until the
 * routine returns, GetExitCodeThread reads its DWORD), and it is reference
 * counted -- CloseHandle before or after the thread ends is fine either way.
 * CREATE_SUSPENDED parks the thread on a gate that ResumeThread opens.
 * SuspendThread of a RUNNING thread is not expressible on POSIX and reports
 * failure ((DWORD)-1), which is the value Win32 code already checks for.
 * -----------------------------------------------------------------------*/
typedef DWORD (*PS3_THREAD_FN)(LPVOID);
typedef PS3_THREAD_FN LPTHREAD_START_ROUTINE;

HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name);
HANDLE CreateEventW(void* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name);
HANDLE CreateEventExA(void* sa, LPCSTR name, DWORD flags, DWORD access);
#define CreateEvent CreateEventA
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
BOOL   PulseEvent(HANDLE h);

HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name);
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG maximum, LPCWSTR name);
HANDLE CreateSemaphoreExA(void* sa, LONG initial, LONG maximum, LPCSTR name, DWORD flags, DWORD access);
#define CreateSemaphore CreateSemaphoreA
BOOL   ReleaseSemaphore(HANDLE h, LONG count, LONG* previous);

HANDLE CreateWaitableTimerA(void* sa, BOOL manual_reset, LPCSTR name);
HANDLE CreateWaitableTimerW(void* sa, BOOL manual_reset, LPCWSTR name);
#define CreateWaitableTimer CreateWaitableTimerA
/* due: negative = relative, in 100 ns units (the only form used in-tree);
 * positive = absolute FILETIME. period in ms, 0 = one-shot. */
BOOL   SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms,
                        void* completion, void* arg, BOOL resume);
BOOL   CancelWaitableTimer(HANDLE h);

HANDLE CreateThread(void* sa, SIZE_T stack, PS3_THREAD_FN fn, LPVOID arg,
                    DWORD flags, DWORD* out_tid);
uintptr_t _beginthreadex(void* sa, unsigned stack, unsigned (*fn)(void*),
                         void* arg, unsigned flags, unsigned* out_tid);
void   ExitThread(DWORD code);
void   _endthreadex(unsigned code);
BOOL   GetExitCodeThread(HANDLE h, DWORD* code);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   SetThreadPriority(HANDLE h, int priority);
int    GetThreadPriority(HANDLE h);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
DWORD  SetThreadIdealProcessor(HANDLE h, DWORD ideal);
LONG   SetThreadDescription(HANDLE h, PCWSTR description);   /* HRESULT */
BOOL   SetPriorityClass(HANDLE process, DWORD cls);
DWORD  GetPriorityClass(HANDLE process);

DWORD  WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD  WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms);
BOOL   CloseHandle(HANDLE h);

/* ---------------------------------------------------------------------------
 * WaitOnAddress / WakeByAddress
 *
 * A parking lot: the address hashes to one of a fixed set of mutex+condvar
 * buckets. The waiter compares under the bucket lock and sleeps on it; the
 * waker takes the same lock and broadcasts. That ordering is what rules out a
 * lost wakeup, provided the waker changes the value BEFORE it wakes -- which
 * is what the Win32 contract requires of it too. WakeByAddressSingle also
 * broadcasts: with hashing, a signal could wake a waiter on a different
 * address and leave the intended one asleep, and a spurious return is
 * something every WaitOnAddress caller already re-checks for.
 * -----------------------------------------------------------------------*/
BOOL   WaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD ms);
void   WakeByAddressSingle(PVOID address);
void   WakeByAddressAll(PVOID address);

/* ---------------------------------------------------------------------------
 * Virtual memory
 *
 * MEM_RESERVE maps PROT_NONE (at the requested address if one is given, and
 * fails rather than displacing an existing mapping, as Win32 does); MEM_COMMIT
 * on a reserved range makes it accessible; MEM_RELEASE unmaps a reservation
 * (its size is remembered, since munmap needs one and Win32 callers pass 0).
 * VirtualProtect cannot report the previous protection -- there is no
 * portable page-table read -- so *old is PAGE_READWRITE, and a caller that
 * restores it gets a writable page, which is the safe direction.
 * -----------------------------------------------------------------------*/
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL   VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL   VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old);
void   GetSystemInfo(SYSTEM_INFO* si);
#define GetNativeSystemInfo GetSystemInfo

/* ---------------------------------------------------------------------------
 * Odds and ends
 * -----------------------------------------------------------------------*/
static inline void OutputDebugStringA(LPCSTR s) { if (s) fputs(s, stderr); }
static inline BOOL IsDebuggerPresent(void)      { return FALSE; }
static inline void DebugBreak(void)             { __builtin_trap(); }
static inline DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char* v = name ? getenv(name) : NULL;
    if (!v) return 0;
    size_t n = strlen(v);
    if (!buf || size == 0 || n + 1 > size) return (DWORD)(n + 1);
    memcpy(buf, v, n + 1);
    return (DWORD)n;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* !_WIN32 */
#endif /* PS3RECOMP_WIN32_COMPAT_H */
