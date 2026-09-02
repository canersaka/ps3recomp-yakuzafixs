/*
 * ps3recomp - Win32 waitable handles, WaitOnAddress and VirtualAlloc on POSIX
 *
 * The half of win32_compat.h that needs state shared across translation
 * units. See the header for the contract of each function; this file is about
 * how they are kept correct.
 *
 * One lock, s_lock, guards every waitable object. That is what makes
 * WaitForMultipleObjects(bWaitAll) mean what Win32 says it means -- either all
 * of the objects are consumed together or none is -- without a lock-ordering
 * scheme across objects. Each object has its own condition variable, used with
 * that shared mutex, so a SetEvent wakes only the threads waiting on that
 * event; a separate condition variable serves multi-object waiters, and every
 * state change broadcasts it only while such a waiter exists.
 *
 * Threads are created detached and tracked by a reference-counted object: one
 * reference belongs to the handle, one to the running thread, and whichever
 * lets go last frees it. That is why a thread routine may return after its
 * handle was closed, and why CloseHandle right after CreateThread -- the
 * "spawn and forget" pattern all over this tree -- leaks nothing.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE          /* pthread_setaffinity_np, CPU_SET */
#endif

#include "win32_compat.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#if defined(__APPLE__)
#  include <pthread/qos.h>
#endif

/* ---------------------------------------------------------------------------
 * Last error
 * -----------------------------------------------------------------------*/
static _Thread_local DWORD t_last_error;
DWORD GetLastError(void)     { return t_last_error; }
void  SetLastError(DWORD e)  { t_last_error = e; }

/* ---------------------------------------------------------------------------
 * Waitable objects
 * -----------------------------------------------------------------------*/
enum { OBJ_EVENT = 1, OBJ_SEMAPHORE, OBJ_THREAD, OBJ_TIMER };
#define OBJ_MAGIC 0x57333234u   /* 'W324' */

typedef struct ps3_obj {
    uint32_t       magic;
    int            kind;
    int            refs;
    pthread_cond_t cv;                 /* used with s_lock */

    /* event */
    int            manual, signaled;

    /* semaphore */
    LONG           count, maximum;

    /* thread */
    pthread_t      pt;
    DWORD          tid;                /* kernel tid, known once it runs */
    int            started, done, suspend_count;
    int            priority;           /* THREAD_PRIORITY_*, applied at start too */
    DWORD_PTR      affinity;           /* 0 = unset */
    DWORD          exit_code;
    PS3_THREAD_FN  fn;
    unsigned     (*fn_crt)(void*);
    LPVOID         arg;

    /* waitable timer */
    unsigned long long due_ns;         /* CLOCK_MONOTONIC */
    unsigned long long period_ns;
    int            timer_set;
} ps3_obj;

static pthread_mutex_t s_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_multi = PTHREAD_COND_INITIALIZER;
static int             s_multi_waiters;

static _Thread_local ps3_obj* t_current_thread;   /* set by the trampoline */

static ps3_obj* obj_new(int kind)
{
    ps3_obj* o = (ps3_obj*)calloc(1, sizeof *o);
    if (!o) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    o->magic = OBJ_MAGIC;
    o->kind  = kind;
    o->refs  = 1;
    pthread_cond_init(&o->cv, NULL);
    return o;
}

static ps3_obj* obj_of(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentThread() || h == GetCurrentProcess())
        return NULL;
    ps3_obj* o = (ps3_obj*)h;
    return o->magic == OBJ_MAGIC ? o : NULL;
}

/* Call with s_lock held. */
static void obj_release_locked(ps3_obj* o)
{
    if (--o->refs > 0) return;
    o->magic = 0;
    pthread_cond_destroy(&o->cv);
    free(o);
}

/* Something a waiter might be waiting for changed. Call with s_lock held. */
static void obj_changed_locked(ps3_obj* o)
{
    pthread_cond_broadcast(&o->cv);
    if (s_multi_waiters) pthread_cond_broadcast(&s_multi);
}

static unsigned long long mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

/* Is the object signaled right now? For a timer that means its due time has
 * passed, which is why timed waits below also wake at the earliest due time.
 * Call with s_lock held. */
static int obj_signaled_locked(const ps3_obj* o, unsigned long long now)
{
    switch (o->kind) {
    case OBJ_EVENT:     return o->signaled;
    case OBJ_SEMAPHORE: return o->count > 0;
    case OBJ_THREAD:    return o->done;
    case OBJ_TIMER:     return o->timer_set && now >= o->due_ns;
    }
    return 0;
}

/* Take the object: what a successful wait does to it. Call with s_lock held. */
static void obj_consume_locked(ps3_obj* o, unsigned long long now)
{
    switch (o->kind) {
    case OBJ_EVENT:     if (!o->manual) o->signaled = 0; break;
    case OBJ_SEMAPHORE: o->count--; break;
    case OBJ_THREAD:    break;
    case OBJ_TIMER:
        if (o->period_ns) {
            /* Periodic: advance past now so a late waiter does not see a
             * burst of stale periods, but keep the phase. */
            do o->due_ns += o->period_ns; while (o->due_ns <= now);
        } else if (!o->manual) {
            o->timer_set = 0;
        }
        break;
    }
}

/* The earliest time at which `o` could become signaled by the clock alone,
 * or 0 if never. Call with s_lock held. */
static unsigned long long obj_next_due_locked(const ps3_obj* o)
{
    return (o->kind == OBJ_TIMER && o->timer_set) ? o->due_ns : 0ull;
}

/* Wait on `cv` (with s_lock) until `deadline_ns` (monotonic; 0 = forever),
 * capped by `due_ns` when non-zero. Returns 0 on wake, ETIMEDOUT past the
 * deadline. Call with s_lock held. */
static int wait_until_locked(pthread_cond_t* cv, unsigned long long deadline_ns,
                             unsigned long long due_ns)
{
    unsigned long long target = deadline_ns;
    if (due_ns && (!target || due_ns < target)) target = due_ns;
    if (!target) return pthread_cond_wait(cv, &s_lock);

    unsigned long long now = mono_ns();
    if (deadline_ns && now >= deadline_ns) return ETIMEDOUT;
    if (now >= target) return 0;               /* a timer came due: re-check */

    /* pthread_cond_timedwait takes an absolute CLOCK_REALTIME time (Darwin has
     * no condattr clock), so convert the monotonic remainder. */
    unsigned long long remain = target - now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(remain / 1000000000ull);
    ts.tv_nsec += (long)(remain % 1000000000ull);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int rc = pthread_cond_timedwait(cv, &s_lock, &ts);
    /* Only the caller's own deadline is a timeout. Waking at a timer's due
     * time, or early through REALTIME/MONOTONIC jitter, means "look again". */
    if (rc == ETIMEDOUT && (!deadline_ns || mono_ns() < deadline_ns)) rc = 0;
    return rc;
}

static unsigned long long deadline_from_ms(DWORD ms)
{
    return ms == INFINITE ? 0ull : mono_ns() + (unsigned long long)ms * 1000000ull;
}

/* --- events ---------------------------------------------------------------- */
HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
    (void)sa; (void)name;
    ps3_obj* o = obj_new(OBJ_EVENT);
    if (!o) return NULL;
    o->manual   = manual_reset ? 1 : 0;
    o->signaled = initial_state ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(void* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name)
{ (void)name; return CreateEventA(sa, manual_reset, initial_state, NULL); }
HANDLE CreateEventExA(void* sa, LPCSTR name, DWORD flags, DWORD access)
{
    (void)access;
    return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET) != 0,
                        (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}
BOOL SetEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 1;
    obj_changed_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL ResetEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 0;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL PulseEvent(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_EVENT) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->signaled = 1;
    obj_changed_locked(o);
    o->signaled = 0;   /* waiters already woken re-check and consume nothing; Win32 PulseEvent is that unreliable too */
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- semaphores ------------------------------------------------------------ */
HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    if (initial < 0 || maximum <= 0 || initial > maximum) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    ps3_obj* o = obj_new(OBJ_SEMAPHORE);
    if (!o) return NULL;
    o->count   = initial;
    o->maximum = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG maximum, LPCWSTR name)
{ (void)name; return CreateSemaphoreA(sa, initial, maximum, NULL); }
HANDLE CreateSemaphoreExA(void* sa, LONG initial, LONG maximum, LPCSTR name, DWORD flags, DWORD access)
{ (void)flags; (void)access; return CreateSemaphoreA(sa, initial, maximum, name); }
BOOL ReleaseSemaphore(HANDLE h, LONG count, LONG* previous)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_SEMAPHORE || count <= 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    if (o->count + count > o->maximum) {
        pthread_mutex_unlock(&s_lock);
        SetLastError(ERROR_INVALID_PARAMETER);       /* ERROR_TOO_MANY_POSTS on Win32 */
        return FALSE;
    }
    if (previous) *previous = o->count;
    o->count += count;
    obj_changed_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- waitable timers ------------------------------------------------------- */
HANDLE CreateWaitableTimerA(void* sa, BOOL manual_reset, LPCSTR name)
{
    (void)sa; (void)name;
    ps3_obj* o = obj_new(OBJ_TIMER);
    if (!o) return NULL;
    o->manual = manual_reset ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateWaitableTimerW(void* sa, BOOL manual_reset, LPCWSTR name)
{ (void)name; return CreateWaitableTimerA(sa, manual_reset, NULL); }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms,
                      void* completion, void* arg, BOOL resume)
{
    (void)completion; (void)arg; (void)resume;
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_TIMER || !due) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    unsigned long long now = mono_ns();
    unsigned long long due_ns;
    if (due->QuadPart <= 0) {
        due_ns = now + (unsigned long long)(-due->QuadPart) * 100ull;   /* relative, 100 ns units */
    } else {
        /* Absolute FILETIME: distance from the wall clock, applied to the
         * monotonic clock. */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        unsigned long long wall = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        unsigned long long tgt  = (unsigned long long)due->QuadPart;
        due_ns = tgt > wall ? now + (tgt - wall) * 100ull : now;
    }
    pthread_mutex_lock(&s_lock);
    o->due_ns    = due_ns;
    o->period_ns = period_ms > 0 ? (unsigned long long)period_ms * 1000000ull : 0ull;
    o->timer_set = 1;
    obj_changed_locked(o);       /* waiters re-arm their deadline against the new due time */
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE h)
{
    ps3_obj* o = obj_of(h);
    if (!o || o->kind != OBJ_TIMER) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    o->timer_set = 0;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* --- threads --------------------------------------------------------------- */

/* Apply a THREAD_PRIORITY_* level to the calling thread (or `pt`/`tid` when
 * it is another one). Best effort on every host: the point is to keep the
 * PPU, SPU-worker and RSX threads ahead of housekeeping, not to promise a
 * scheduler class. */
static void apply_priority(pthread_t pt, DWORD tid, int level, int is_self)
{
#if defined(__APPLE__)
    struct sched_param sp; int policy = SCHED_OTHER;
    if (pthread_getschedparam(pt, &policy, &sp) == 0) {
        int lo = sched_get_priority_min(policy), hi = sched_get_priority_max(policy);
        int def = (lo + hi) / 2;
        int p;
        if      (level >= THREAD_PRIORITY_TIME_CRITICAL) p = hi;
        else if (level <= THREAD_PRIORITY_IDLE)          p = lo;
        else                                             p = def + level * ((hi - def) / 4);
        if (p < lo) p = lo;
        if (p > hi) p = hi;
        sp.sched_priority = p;
        pthread_setschedparam(pt, policy, &sp);
    }
    /* QoS is what actually decides P-core vs E-core on Apple Silicon. Only the
     * calling thread can set its own; a thread created suspended gets it from
     * the trampoline, which runs on the new thread. */
    if (is_self) {
        qos_class_t q = QOS_CLASS_DEFAULT;
        if      (level >= THREAD_PRIORITY_ABOVE_NORMAL) q = QOS_CLASS_USER_INTERACTIVE;
        else if (level == THREAD_PRIORITY_NORMAL)       q = QOS_CLASS_USER_INITIATED;
        else if (level >= THREAD_PRIORITY_LOWEST)       q = QOS_CLASS_UTILITY;
        else                                            q = QOS_CLASS_BACKGROUND;
        pthread_set_qos_class_self_np(q, 0);
    }
    (void)tid;
#elif defined(__linux__)
    /* SCHED_OTHER has no per-thread priority, only nice, which Linux applies
     * per thread through setpriority(PRIO_PROCESS, tid). Lowering nice below
     * zero needs CAP_SYS_NICE or an RLIMIT_NICE grant; without it the call
     * fails and the thread simply stays at its inherited level. */
    int nice;
    if      (level >= THREAD_PRIORITY_TIME_CRITICAL) nice = -20;
    else if (level >= THREAD_PRIORITY_HIGHEST)       nice = -10;
    else if (level >= THREAD_PRIORITY_ABOVE_NORMAL)  nice = -5;
    else if (level == THREAD_PRIORITY_NORMAL)        nice = 0;
    else if (level >= THREAD_PRIORITY_BELOW_NORMAL)  nice = 5;
    else if (level >= THREAD_PRIORITY_LOWEST)        nice = 10;
    else                                             nice = 19;
    if (is_self) tid = (DWORD)syscall(SYS_gettid);
    if (tid) setpriority(PRIO_PROCESS, (id_t)tid, nice);
    (void)pt;
#else
    (void)pt; (void)tid; (void)level; (void)is_self;
#endif
}

static void apply_affinity(pthread_t pt, DWORD_PTR mask)
{
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set);
    for (unsigned i = 0; i < 8 * sizeof mask && i < CPU_SETSIZE; i++)
        if (mask & ((DWORD_PTR)1 << i)) CPU_SET(i, &set);
    pthread_setaffinity_np(pt, sizeof set, &set);
#else
    /* Apple Silicon has no thread affinity; QoS (above) is the lever there. */
    (void)pt; (void)mask;
#endif
}

static void thread_finish(ps3_obj* t, DWORD code)
{
    pthread_mutex_lock(&s_lock);
    t->exit_code = code;
    t->done = 1;
    obj_changed_locked(t);
    obj_release_locked(t);       /* the running thread's reference */
    pthread_mutex_unlock(&s_lock);
}

static void* thread_trampoline(void* p)
{
    ps3_obj* t = (ps3_obj*)p;
    t_current_thread = t;

    pthread_mutex_lock(&s_lock);
    t->tid = GetCurrentThreadId();
    while (t->suspend_count > 0)                 /* CREATE_SUSPENDED gate */
        pthread_cond_wait(&t->cv, &s_lock);
    t->started = 1;
    int prio = t->priority; DWORD_PTR aff = t->affinity;
    pthread_mutex_unlock(&s_lock);

    if (prio != THREAD_PRIORITY_NORMAL) apply_priority(pthread_self(), t->tid, prio, 1);
    if (aff) apply_affinity(pthread_self(), aff);

    DWORD rc = t->fn ? t->fn(t->arg) : (DWORD)t->fn_crt(t->arg);
    t_current_thread = NULL;
    thread_finish(t, rc);
    return NULL;
}

static HANDLE create_thread(SIZE_T stack, PS3_THREAD_FN fn, unsigned (*fn_crt)(void*),
                            LPVOID arg, DWORD flags, DWORD* out_tid)
{
    if (out_tid) *out_tid = 0;
    ps3_obj* t = obj_new(OBJ_THREAD);
    if (!t) return NULL;
    t->refs   = 2;               /* the handle, and the thread itself */
    t->fn     = fn;
    t->fn_crt = fn_crt;
    t->arg    = arg;
    t->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) {
        /* Darwin rejects a size that is not a page multiple; both reject one
         * under the minimum. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        if (page == 0) page = 4096;
        size_t sz = (stack + page - 1) & ~(page - 1);
        if (sz < (size_t)PTHREAD_STACK_MIN) sz = ((size_t)PTHREAD_STACK_MIN + page - 1) & ~(page - 1);
        pthread_attr_setstacksize(&attr, sz);
    }
    int rc = pthread_create(&t->pt, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        t->magic = 0; pthread_cond_destroy(&t->cv); free(t);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return (HANDLE)t;
}

HANDLE CreateThread(void* sa, SIZE_T stack, PS3_THREAD_FN fn, LPVOID arg, DWORD flags, DWORD* out_tid)
{ (void)sa; return create_thread(stack, fn, NULL, arg, flags, out_tid); }

uintptr_t _beginthreadex(void* sa, unsigned stack, unsigned (*fn)(void*), void* arg,
                         unsigned flags, unsigned* out_tid)
{
    (void)sa;
    DWORD tid = 0;
    HANDLE h = create_thread(stack, NULL, fn, arg, flags, &tid);
    if (out_tid) *out_tid = tid;
    return (uintptr_t)h;
}

void ExitThread(DWORD code)
{
    ps3_obj* t = t_current_thread;
    t_current_thread = NULL;
    if (t) thread_finish(t, code);
    pthread_exit(NULL);
}
void _endthreadex(unsigned code) { ExitThread((DWORD)code); }

BOOL GetExitCodeThread(HANDLE h, DWORD* code)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD || !code) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    *code = t->done ? t->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    pthread_mutex_lock(&s_lock);
    DWORD prev = (DWORD)t->suspend_count;
    if (t->suspend_count > 0 && --t->suspend_count == 0) pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&s_lock);
    return prev;
}

/* Only a thread that has not started yet can be held: bumping its gate count
 * is exact. Stopping a running pthread from outside is not a thing POSIX
 * offers, so that case reports failure the way the Win32 callers already
 * handle. */
DWORD SuspendThread(HANDLE h)
{
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    pthread_mutex_lock(&s_lock);
    DWORD r = (DWORD)-1;
    if (!t->started) { r = (DWORD)t->suspend_count; t->suspend_count++; }
    pthread_mutex_unlock(&s_lock);
    if (r == (DWORD)-1) SetLastError(ERROR_INVALID_PARAMETER);
    return r;
}

/* Resolve a thread handle (real or the pseudo-handle) for the priority and
 * naming calls. Returns 0 if the handle is not a thread. */
static int resolve_thread(HANDLE h, pthread_t* pt, DWORD* tid, int* is_self, ps3_obj** obj)
{
    *obj = NULL;
    if (h == GetCurrentThread()) {
        *pt = pthread_self(); *tid = GetCurrentThreadId(); *is_self = 1;
        *obj = t_current_thread;
        return 1;
    }
    ps3_obj* t = obj_of(h);
    if (!t || t->kind != OBJ_THREAD) return 0;
    *pt = t->pt; *tid = t->tid; *is_self = (t == t_current_thread); *obj = t;
    return 1;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    int started = 1;
    if (t) {
        pthread_mutex_lock(&s_lock);
        t->priority = priority;
        started = t->started;
        pthread_mutex_unlock(&s_lock);
    }
    if (started) apply_priority(pt, tid, priority, self);   /* else the trampoline applies it */
    return TRUE;
}

int GetThreadPriority(HANDLE h)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return THREAD_PRIORITY_ERROR_RETURN; }
    if (!t) return THREAD_PRIORITY_NORMAL;
    pthread_mutex_lock(&s_lock);
    int p = t->priority;
    pthread_mutex_unlock(&s_lock);
    return p;
}

DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t) || !mask) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    DWORD_PTR prev = t && t->affinity ? t->affinity : (DWORD_PTR)1;
    int started = 1;
    if (t) {
        pthread_mutex_lock(&s_lock);
        t->affinity = mask;
        started = t->started;
        pthread_mutex_unlock(&s_lock);
    }
    if (started) apply_affinity(pt, mask);
    return prev;
}

DWORD SetThreadIdealProcessor(HANDLE h, DWORD ideal)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    (void)ideal;
    return 0;   /* advisory on Windows too; the previous "ideal" is unknowable here */
}

LONG SetThreadDescription(HANDLE h, PCWSTR description)
{
    pthread_t pt; DWORD tid; int self; ps3_obj* t;
    if (!resolve_thread(h, &pt, &tid, &self, &t)) return (LONG)0x80070006L;   /* E_HANDLE */
    char name[64] = {0};
    if (description) {
        size_t n = wcstombs(name, description, sizeof name - 1);
        if (n == (size_t)-1) name[0] = '\0';
    }
#if defined(__APPLE__)
    if (self) pthread_setname_np(name);             /* Darwin names only the caller */
#elif defined(__linux__)
    char short_name[16];                            /* the kernel's TASK_COMM_LEN */
    size_t n = strnlen(name, sizeof short_name - 1);
    memcpy(short_name, name, n);
    short_name[n] = '\0';
    pthread_setname_np(pt, short_name);
#else
    (void)pt;
#endif
    return 0;   /* S_OK */
}

BOOL  SetPriorityClass(HANDLE process, DWORD cls) { (void)process; (void)cls; return TRUE; }
DWORD GetPriorityClass(HANDLE process)            { (void)process; return NORMAL_PRIORITY_CLASS; }

/* --- waits ----------------------------------------------------------------- */
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    ps3_obj* o = obj_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    unsigned long long deadline = deadline_from_ms(ms);
    DWORD result = WAIT_TIMEOUT;

    pthread_mutex_lock(&s_lock);
    o->refs++;                   /* the object must outlive the wait even if closed meanwhile */
    for (;;) {
        unsigned long long now = mono_ns();
        if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; break; }
        if (ms == 0) break;
        if (wait_until_locked(&o->cv, deadline, obj_next_due_locked(o)) == ETIMEDOUT) {
            /* One last look: the state may have changed exactly at the deadline. */
            now = mono_ns();
            if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; }
            break;
        }
    }
    obj_release_locked(o);
    pthread_mutex_unlock(&s_lock);
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return result;
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{ (void)alertable; return WaitForSingleObject(h, ms); }

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms)
{
    enum { MAX_WAIT = 64 };   /* MAXIMUM_WAIT_OBJECTS */
    if (count == 0 || count > MAX_WAIT || !handles) { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    ps3_obj* objs[MAX_WAIT];
    for (DWORD i = 0; i < count; i++) {
        objs[i] = obj_of(handles[i]);
        if (!objs[i]) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    }
    unsigned long long deadline = deadline_from_ms(ms);
    DWORD result = WAIT_TIMEOUT;

    pthread_mutex_lock(&s_lock);
    for (DWORD i = 0; i < count; i++) objs[i]->refs++;
    s_multi_waiters++;
    for (;;) {
        unsigned long long now = mono_ns();
        if (wait_all) {
            DWORD ready = 0;
            for (DWORD i = 0; i < count; i++) ready += obj_signaled_locked(objs[i], now) ? 1 : 0;
            if (ready == count) {
                for (DWORD i = 0; i < count; i++) obj_consume_locked(objs[i], now);
                result = WAIT_OBJECT_0;
                break;
            }
        } else {
            DWORD hit = count;
            for (DWORD i = 0; i < count; i++)
                if (obj_signaled_locked(objs[i], now)) { hit = i; break; }
            if (hit < count) { obj_consume_locked(objs[hit], now); result = WAIT_OBJECT_0 + hit; break; }
        }
        if (ms == 0) break;
        unsigned long long due = 0;
        for (DWORD i = 0; i < count; i++) {
            unsigned long long d = obj_next_due_locked(objs[i]);
            if (d && (!due || d < due)) due = d;
        }
        if (wait_until_locked(&s_multi, deadline, due) == ETIMEDOUT) break;
    }
    s_multi_waiters--;
    for (DWORD i = 0; i < count; i++) obj_release_locked(objs[i]);
    pthread_mutex_unlock(&s_lock);
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return result;
}

BOOL CloseHandle(HANDLE h)
{
    if (h == GetCurrentThread() || h == GetCurrentProcess()) return TRUE;   /* pseudo-handles */
    ps3_obj* o = obj_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&s_lock);
    obj_release_locked(o);
    pthread_mutex_unlock(&s_lock);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * WaitOnAddress / WakeByAddress
 * -----------------------------------------------------------------------*/
#define PL_BUCKETS 64
static struct { pthread_mutex_t m; pthread_cond_t c; } s_pl[PL_BUCKETS];
static pthread_once_t s_pl_once = PTHREAD_ONCE_INIT;

static void pl_init(void)
{
    for (int i = 0; i < PL_BUCKETS; i++) {
        pthread_mutex_init(&s_pl[i].m, NULL);
        pthread_cond_init(&s_pl[i].c, NULL);
    }
}
static unsigned pl_bucket(const volatile void* a)
{
    uintptr_t x = (uintptr_t)a;
    x ^= x >> 17; x *= 0x9E3779B1u; x ^= x >> 13;
    return (unsigned)(x % PL_BUCKETS);
}
static int pl_equal(const volatile void* a, const void* cmp, size_t size)
{
    switch (size) {
    case 1: return *(const volatile uint8_t*)a  == *(const uint8_t*)cmp;
    case 2: return *(const volatile uint16_t*)a == *(const uint16_t*)cmp;
    case 4: return *(const volatile uint32_t*)a == *(const uint32_t*)cmp;
    case 8: return *(const volatile uint64_t*)a == *(const uint64_t*)cmp;
    default: return memcmp((const void*)a, cmp, size) == 0;
    }
}

BOOL WaitOnAddress(volatile VOID* address, PVOID compare, SIZE_T size, DWORD ms)
{
    if (!address || !compare || (size != 1 && size != 2 && size != 4 && size != 8)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_once(&s_pl_once, pl_init);
    unsigned b = pl_bucket(address);
    pthread_mutex_lock(&s_pl[b].m);
    BOOL ok = TRUE;
    if (pl_equal(address, compare, size)) {
        int rc;
        if (ms == INFINITE) rc = pthread_cond_wait(&s_pl[b].c, &s_pl[b].m);
        else { struct timespec ts = ps3__deadline_ms(ms); rc = pthread_cond_timedwait(&s_pl[b].c, &s_pl[b].m, &ts); }
        /* Woken (by a wake on this bucket, or spuriously): report success and
         * let the caller re-check, as the Win32 contract already demands. A
         * timeout with the value now different is still a success. */
        if (rc == ETIMEDOUT && pl_equal(address, compare, size)) ok = FALSE;
    }
    pthread_mutex_unlock(&s_pl[b].m);
    if (!ok) SetLastError(ERROR_TIMEOUT);
    return ok;
}

void WakeByAddressAll(PVOID address)
{
    pthread_once(&s_pl_once, pl_init);
    unsigned b = pl_bucket(address);
    pthread_mutex_lock(&s_pl[b].m);
    pthread_cond_broadcast(&s_pl[b].c);
    pthread_mutex_unlock(&s_pl[b].m);
}
void WakeByAddressSingle(PVOID address) { WakeByAddressAll(address); }   /* see the header */

/* ---------------------------------------------------------------------------
 * Virtual memory
 * -----------------------------------------------------------------------*/
#define VA_REGIONS 512
static struct { void* base; size_t size; } s_regions[VA_REGIONS];

static int prot_of(DWORD protect)
{
    switch (protect & 0xFFu) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_NONE;
    }
}

static size_t page_size(void)
{
    long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? (size_t)v : 4096u;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    size_t pg = page_size();
    uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
    size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
    int prot = (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE;

    if (type & MEM_RESERVE) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
        if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
        void* p = mmap(address ? (void*)a0 : NULL, sz, prot, flags, -1, 0);
        if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        if (address && p != (void*)a0) {
            /* The hint was not honoured: the range is in use, which is the
             * Win32 failure. Never displace whatever lives there. */
            munmap(p, sz);
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < VA_REGIONS; i++)
            if (!s_regions[i].base) { s_regions[i].base = p; s_regions[i].size = sz; break; }
        pthread_mutex_unlock(&s_lock);
        return p;
    }
    if (type & MEM_COMMIT) {
        if (!address) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
        if (mprotect((void*)a0, sz, prot) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
        return (LPVOID)a0;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return NULL;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type)
{
    size_t pg = page_size();
    if (type & MEM_RELEASE) {
        BOOL ok = FALSE;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < VA_REGIONS; i++) {
            if (s_regions[i].base == address && s_regions[i].base) {
                munmap(s_regions[i].base, s_regions[i].size);
                s_regions[i].base = NULL; s_regions[i].size = 0;
                ok = TRUE;
                break;
            }
        }
        pthread_mutex_unlock(&s_lock);
        if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
        return ok;
    }
    if (type & MEM_DECOMMIT) {
        uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
        size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
        if (mprotect((void*)a0, sz, PROT_NONE) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        madvise((void*)a0, sz, MADV_DONTNEED);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old)
{
    size_t pg = page_size();
    uintptr_t a0 = (uintptr_t)address & ~(pg - 1);
    size_t    sz = (((uintptr_t)address + size + pg - 1) & ~(pg - 1)) - a0;
    if (old) *old = PAGE_READWRITE;
    if (mprotect((void*)a0, sz, prot_of(protect)) != 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

void GetSystemInfo(SYSTEM_INFO* si)
{
    if (!si) return;
    memset(si, 0, sizeof *si);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    si->dwPageSize              = (DWORD)page_size();
    si->dwAllocationGranularity = si->dwPageSize;
    si->dwNumberOfProcessors    = (DWORD)n;
    si->dwActiveProcessorMask   = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    si->lpMinimumApplicationAddress = (LPVOID)(uintptr_t)si->dwPageSize;
    si->lpMaximumApplicationAddress = (LPVOID)(uintptr_t)0x00007FFFFFFFFFFFull;
}

#else  /* _WIN32 */
/* Nothing to build: the header is a passthrough to <windows.h> there. */
typedef int ps3_win32_compat_not_needed_on_windows;
#endif
