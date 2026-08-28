/*
 * ps3recomp - POSIX functions absent on Darwin
 *
 * macOS ships neither pthread_mutex_timedlock (POSIX 2001) nor sem_timedwait
 * (POSIX realtime). Both are emulated by polling the non-blocking variant
 * against the caller's absolute CLOCK_REALTIME deadline. Contended waits here
 * are rare and short (guest sync primitives with millisecond timeouts), so the
 * poll interval costs less than pulling in a full futex-style reimplementation.
 */
#ifndef PS3RECOMP_DARWIN_COMPAT_H
#define PS3RECOMP_DARWIN_COMPAT_H

#if defined(__APPLE__)

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

/* Poll granularity. 200 us keeps worst-case overshoot well under the 1 ms
 * resolution the guest timeout APIs actually express. */
#define PS3_TIMEDWAIT_POLL_NS  200000L

/* QueryPerformanceCounter/Frequency, for the timing DIAGNOSTICS that measure
 * SPU interpreter throughput. Those call sites are not Windows-specific in
 * intent -- they just used the Win32 clock because that is where they were
 * written -- so shim the three names onto CLOCK_MONOTONIC rather than fence
 * every use site off with #ifdef and lose the diagnostic on macOS.
 *
 * Frequency is reported in nanoseconds so the counter is a plain ns count;
 * callers only ever divide one by the other. */
typedef struct { long long QuadPart; } PS3_LARGE_INTEGER;
#define LARGE_INTEGER PS3_LARGE_INTEGER

static inline int QueryPerformanceCounter(PS3_LARGE_INTEGER* v)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

static inline int QueryPerformanceFrequency(PS3_LARGE_INTEGER* v)
{
    v->QuadPart = 1000000000LL;
    return 1;
}

static inline int ps3__deadline_passed(const struct timespec* abs)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec  != abs->tv_sec) return now.tv_sec > abs->tv_sec;
    return now.tv_nsec >= abs->tv_nsec;
}

static inline void ps3__poll_sleep(void)
{
    struct timespec d = { 0, PS3_TIMEDWAIT_POLL_NS };
    nanosleep(&d, NULL);
}

static inline int pthread_mutex_timedlock(pthread_mutex_t* m,
                                          const struct timespec* abstime)
{
    for (;;) {
        int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;              /* acquired, or a real error */
        if (ps3__deadline_passed(abstime)) return ETIMEDOUT;
        ps3__poll_sleep();
    }
}

static inline int ps3_sem_timedwait(sem_t* s, const struct timespec* abstime)
{
    for (;;) {
        if (sem_trywait(s) == 0) return 0;
        if (errno != EAGAIN) return -1;          /* propagate the real error */
        if (ps3__deadline_passed(abstime)) { errno = ETIMEDOUT; return -1; }
        ps3__poll_sleep();
    }
}
#define sem_timedwait ps3_sem_timedwait

#endif /* __APPLE__ */
#endif /* PS3RECOMP_DARWIN_COMPAT_H */
