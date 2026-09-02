/*
 * ps3recomp - counting semaphore for POSIX hosts
 *
 * <semaphore.h> is the obvious tool and the wrong one: Darwin implements
 * sem_init as a stub that fails with ENOSYS (only named semaphores work), and
 * ships no sem_timedwait at all, which runtime/platform/darwin_compat.h has
 * been emulating by polling. A mutex and a condition variable give exact
 * semantics, a real timed wait, and one code path on every host -- so the
 * Linux CI run exercises the same lines a Mac executes.
 *
 * Deadlines are absolute CLOCK_REALTIME, matching sem_timedwait, so a caller
 * that computed one for sem_timedwait passes it here unchanged.
 */
#ifndef PS3RECOMP_POSIX_SEM_H
#define PS3RECOMP_POSIX_SEM_H

#ifndef _WIN32

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        count;
} ps3_sem_t;

static inline int ps3_sem_init(ps3_sem_t* s, unsigned initial)
{
    int rc = pthread_mutex_init(&s->m, NULL);
    if (rc != 0) { errno = rc; return -1; }
    rc = pthread_cond_init(&s->c, NULL);
    if (rc != 0) { pthread_mutex_destroy(&s->m); errno = rc; return -1; }
    s->count = initial;
    return 0;
}

static inline int ps3_sem_destroy(ps3_sem_t* s)
{
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    return 0;
}

static inline int ps3_sem_post(ps3_sem_t* s)
{
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return 0;
}

static inline int ps3_sem_wait(ps3_sem_t* s)
{
    pthread_mutex_lock(&s->m);
    while (s->count == 0) pthread_cond_wait(&s->c, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
    return 0;
}

static inline int ps3_sem_trywait(ps3_sem_t* s)
{
    pthread_mutex_lock(&s->m);
    int ok = s->count > 0;
    if (ok) s->count--;
    pthread_mutex_unlock(&s->m);
    if (!ok) { errno = EAGAIN; return -1; }
    return 0;
}

static inline int ps3_sem_getvalue(ps3_sem_t* s, int* value)
{
    pthread_mutex_lock(&s->m);
    *value = (int)s->count;
    pthread_mutex_unlock(&s->m);
    return 0;
}

/* Returns 0 on success, -1 with errno = ETIMEDOUT when the deadline passes. */
static inline int ps3_sem_timedwait(ps3_sem_t* s, const struct timespec* abs_deadline)
{
    pthread_mutex_lock(&s->m);
    int rc = 0;
    while (s->count == 0 && rc == 0)
        rc = pthread_cond_timedwait(&s->c, &s->m, abs_deadline);
    int ok = s->count > 0;         /* a post that raced the deadline still counts */
    if (ok) s->count--;
    pthread_mutex_unlock(&s->m);
    if (!ok) { errno = ETIMEDOUT; return -1; }
    return 0;
}

#endif /* !_WIN32 */
#endif /* PS3RECOMP_POSIX_SEM_H */
