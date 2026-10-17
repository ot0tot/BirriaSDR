/* SPDX-License-Identifier: MIT */
#ifndef BIRRIASDR_PLATFORM_H
#define BIRRIASDR_PLATFORM_H

#ifndef _WIN32
#include <pthread.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Only the pthread subset used by this library. SRW locks allow static
 * initialization in the transfer ownership tests and are nonrecursive.
 */
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{ (void)attr; InitializeSRWLock(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{ (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m)
{ AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{ ReleaseSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_trylock(pthread_mutex_t *m)
{ return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY; }
static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
{ (void)attr; InitializeConditionVariable(c); return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c)
{ (void)c; return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c)
{ WakeAllConditionVariable(c); return 0; }
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                        const struct timespec *deadline)
{
    struct timespec now;
    int64_t ns;
    DWORD ms;
    timespec_get(&now, TIME_UTC);
    ns = (int64_t)(deadline->tv_sec - now.tv_sec) * INT64_C(1000000000)
         + deadline->tv_nsec - now.tv_nsec;
    ms = ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000);
    if (SleepConditionVariableSRW(c, m, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}
struct bs_thread_start { void *(*fn)(void *); void *context; };
static unsigned __stdcall bs_thread_entry(void *context)
{
    struct bs_thread_start start = *(struct bs_thread_start *)context;
    free(context);
    start.fn(start.context);
    return 0;
}
/* All workers return NULL and all joins discard their result. */
static inline int pthread_create(pthread_t *thread, const void *attr,
                                  void *(*fn)(void *), void *context)
{
    struct bs_thread_start *start = malloc(sizeof(*start));
    (void)attr;
    if (!start) return ENOMEM;
    start->fn = fn;
    start->context = context;
    *thread = (HANDLE)_beginthreadex(NULL, 0, bs_thread_entry, start, 0, NULL);
    if (!*thread) { free(start); return EAGAIN; }
    return 0;
}
static inline int pthread_join(pthread_t thread, void **result)
{
    DWORD status = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (result) *result = NULL;
    return status == WAIT_OBJECT_0 ? 0 : EINVAL;
}
#endif
#endif
