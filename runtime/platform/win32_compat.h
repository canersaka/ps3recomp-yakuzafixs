/*
 * ps3recomp - Win32 sync/timing compatibility shim for POSIX hosts
 *
 * A handful of translation units were written directly against the Win32
 * synchronisation and timing API (SRWLOCK, CONDITION_VARIABLE, QPC, events).
 * This header supplies those names on POSIX so the call sites compile
 * unchanged. On Windows it is a no-op passthrough to <windows.h>.
 *
 * IMPORTANT -- storage size. Win32 SRWLOCK and CONDITION_VARIABLE are exactly
 * pointer-sized and zero-initialise validly, so several structs (notably
 * spu_context::ch_wait_lock / ch_wait_cv) store them inline in a `void*` slot.
 * pthread_mutex_t (64 B on Darwin) and pthread_cond_t (48 B) do NOT fit there;
 * casting the slot and writing a pthread object through it would overflow into
 * adjacent members. So on POSIX these map to `void*` and the real pthread
 * object is heap-allocated on first use and published into the slot with an
 * atomic CAS. NULL still means "uninitialised", so existing memset-based
 * zero-init remains correct.
 */
#ifndef PS3RECOMP_WIN32_COMPAT_H
#define PS3RECOMP_WIN32_COMPAT_H

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else

#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef FALSE
#  define FALSE 0
#endif
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef INFINITE
#  define INFINITE 0xFFFFFFFFu
#endif

typedef uint32_t           DWORD;
typedef int                BOOL;
typedef long long          LONGLONG;
typedef void*              HANDLE;
typedef union { LONGLONG QuadPart; } LARGE_INTEGER;

/* Pointer-sized handles; the pthread object lives on the heap (see note above). */
typedef void*  SRWLOCK;
typedef void*  CONDITION_VARIABLE;
#define SRWLOCK_INIT            NULL
#define CONDITION_VARIABLE_INIT NULL

/* --- lazy, race-free materialisation of the pthread object into a void* slot -- */
static inline void* ps3_lazy_obj(void** slot, size_t sz,
                                 void (*init)(void*))
{
    _Atomic(void*)* a = (_Atomic(void*)*)slot;
    void* cur = atomic_load_explicit(a, memory_order_acquire);
    if (cur) return cur;
    void* fresh = calloc(1, sz);
    if (!fresh) return NULL;
    init(fresh);
    void* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(a, &expected, fresh,
                                                memory_order_acq_rel,
                                                memory_order_acquire))
        return fresh;
    free(fresh);                 /* lost the race; use the winner's object */
    return expected;
}

static inline void ps3__mtx_init(void* p) { pthread_mutex_init((pthread_mutex_t*)p, NULL); }
static inline void ps3__cv_init (void* p) { pthread_cond_init ((pthread_cond_t*)p,  NULL); }

static inline pthread_mutex_t* ps3_srw(SRWLOCK* l)
{ return (pthread_mutex_t*)ps3_lazy_obj((void**)l, sizeof(pthread_mutex_t), ps3__mtx_init); }
static inline pthread_cond_t*  ps3_cv (CONDITION_VARIABLE* c)
{ return (pthread_cond_t*)ps3_lazy_obj((void**)c, sizeof(pthread_cond_t), ps3__cv_init); }

/* Only the Exclusive variants are used in-tree, so a plain mutex is the correct
 * mapping -- and it is what pthread_cond_wait requires. */
static inline void AcquireSRWLockExclusive(SRWLOCK* l)    { pthread_mutex_lock(ps3_srw(l)); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l)    { pthread_mutex_unlock(ps3_srw(l)); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) { return pthread_mutex_trylock(ps3_srw(l)) == 0; }

static inline void WakeConditionVariable(CONDITION_VARIABLE* c)    { pthread_cond_signal(ps3_cv(c)); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE* c) { pthread_cond_broadcast(ps3_cv(c)); }

static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l,
                                             DWORD ms, unsigned long flags)
{
    (void)flags;
    pthread_cond_t*  cv = ps3_cv(c);
    pthread_mutex_t* mx = ps3_srw(l);
    if (ms == INFINITE) return pthread_cond_wait(cv, mx) == 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, mx, &ts) == 0;
}

/* --- timing ------------------------------------------------------------- */
static inline unsigned long long ps3__mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
static inline unsigned long long GetTickCount64(void) { return ps3__mono_ns() / 1000000ull; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter  (LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)ps3__mono_ns(); return TRUE; }

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

/* --- auto-reset event (only the create/set/wait subset used in-tree) ------ */
typedef struct { pthread_mutex_t m; pthread_cond_t c; int signaled; int manual; } ps3_event;

static inline HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, const char* name)
{
    (void)sa; (void)name;
    ps3_event* e = (ps3_event*)calloc(1, sizeof(ps3_event));
    if (!e) return NULL;
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->signaled = initial ? 1 : 0;
    e->manual   = manual ? 1 : 0;
    return (HANDLE)e;
}
static inline BOOL SetEvent(HANDLE h)
{
    ps3_event* e = (ps3_event*)h;
    if (!e) return FALSE;
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    if (e->manual) pthread_cond_broadcast(&e->c); else pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
    return TRUE;
}

static inline void Sleep(unsigned long ms)
{
    struct timespec d = { (time_t)(ms / 1000ul), (long)(ms % 1000ul) * 1000000L };
    nanosleep(&d, NULL);
}

/* --- threads ------------------------------------------------------------- */
#define WINAPI
typedef void* LPVOID;
typedef DWORD (*PS3_THREAD_FN)(LPVOID);

typedef struct { PS3_THREAD_FN fn; LPVOID arg; } ps3__thunk;

static inline void* ps3__thread_trampoline(void* p)
{
    ps3__thunk t = *(ps3__thunk*)p;
    free(p);
    t.fn(t.arg);                 /* Win32 returns DWORD; pthread wants void* */
    return NULL;
}

/* Only the "spawn and forget" subset used in-tree: the returned handle is not
 * waited on or closed anywhere, so the thread is detached. */
static inline HANDLE CreateThread(void* sa, size_t stack, PS3_THREAD_FN fn,
                                  LPVOID arg, DWORD flags, DWORD* out_tid)
{
    (void)sa; (void)flags;
    if (out_tid) *out_tid = 0;
    ps3__thunk* t = (ps3__thunk*)calloc(1, sizeof(ps3__thunk));
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&attr, stack < PTHREAD_STACK_MIN ? PTHREAD_STACK_MIN : stack);

    pthread_t th;
    int rc = pthread_create(&th, &attr, ps3__thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return NULL; }
    return (HANDLE)1;            /* non-NULL "succeeded"; never dereferenced */
}

#endif /* !_WIN32 */
#endif /* PS3RECOMP_WIN32_COMPAT_H */
