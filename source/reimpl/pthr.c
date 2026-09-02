/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define PTHR_MAX_OBJECTS 1024

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

void * initializedObjects[PTHR_MAX_OBJECTS] = {0};
static SceKernelLwMutexWork pthr_mutex;

// 0 = not created, 1 = a thread is creating it right now, 2 = ready to use.
enum { PTHR_META_UNINIT = 0, PTHR_META_CREATING = 1, PTHR_META_READY = 2 };
static atomic_int pthr_mutex_state = PTHR_META_UNINIT;

// The meta-lock's own lazy creation must be race-free, same as the bionic
// mutex/cond structs it protects: two Gangster2 threads hitting PTHR_LOCK for
// the very first time concurrently (e.g. two threads racing on a libstdc++
// std::locale guard, as before) would otherwise both see pthr_mutex_state
// unset and both call sceKernelCreateLwMutex() on the SAME SceKernelLwMutexWork
// concurrently, corrupting it -- which silently defeats the mutual exclusion
// PTHR_LOCK/PTHR_UNLOCK is supposed to provide for `_mutex_t_static_init()`/
// `_cond_t_static_init()`, reopening the exact real_ptr race that crashed
// PSVGMV002 in gangstarmiamivindication-psp2core-1788232234 and again in
// -1788241153 with the identical stack (real pthread_mutex_unlock() jumping
// to a garbage address read from a corrupted mutex object).
#define PTHR_LOCK \
    { \
        int expected = PTHR_META_UNINIT; \
        if (atomic_compare_exchange_strong(&pthr_mutex_state, &expected, PTHR_META_CREATING)) { \
            int ret = sceKernelCreateLwMutex(&pthr_mutex, "pthr_lock", 0, 0, NULL); \
            if (ret < 0) { \
                sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
                atomic_store(&pthr_mutex_state, PTHR_META_UNINIT); \
                return 0; \
            } \
            atomic_store(&pthr_mutex_state, PTHR_META_READY); \
        } else { \
            while (atomic_load(&pthr_mutex_state) != PTHR_META_READY) { \
                sceKernelDelayThread(100); \
            } \
        } \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (atomic_load(&pthr_mutex_state) == PTHR_META_READY) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

int isObjectInitialized(const void * mut) {
    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) {
            PTHR_UNLOCK
            return 1;
        }
    }
    PTHR_UNLOCK
    return 0;
}

int rememberObject(void * mut) {
    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == 0) {
            initializedObjects[i] = mut;
            PTHR_UNLOCK
            return 1;
        }
    }
    PTHR_UNLOCK
    return 0;
}

int forgetObject(const void * mut) {
    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) {
            initializedObjects[i] = 0;
            PTHR_UNLOCK
            return 1;
        }
    }
    PTHR_UNLOCK
    return 0;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
//
// The whole check-then-initialize sequence must run under a single, continuous
// critical section: two threads racing to lazily initialize the SAME statically-
// initialized (all-zero) Bionic mutex for the first time -- e.g. two Gangster2
// game threads both hitting a libstdc++ std::locale guard mutex for the first
// time concurrently -- would otherwise both pass the "not yet initialized" check,
// both malloc+init their own pthread_mutex_t, and both overwrite `mutex->real_ptr`,
// leaving one thread's lock held on a buffer nothing points to anymore and the
// other thread unlocking a different, half-initialized buffer. That's exactly
// what crashed PSVGMV002 in gangstarmiamivindication-psp2core-1788232234:
// Data abort inside the real pthread_mutex_unlock() with a garbage jump target.
// See CURATED entry / port_progress.md Fase 4 for the confirmed root cause.
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mutex) {
            PTHR_UNLOCK
            return 0;
        }
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t mut;
    mutex->real_ptr = malloc(sizeof(pthread_mutex_t));
    sceClibMemcpy(mutex->real_ptr, &mut, sizeof(pthread_mutex_t));

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(mutex->real_ptr, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
            if (initializedObjects[i] == 0) {
                initializedObjects[i] = mutex;
                break;
            }
        }
        l_checkpoint(11, "pthr: first-time lazy mutex init %p from thread 0x%x", mutex, sceKernelGetThreadId());
    } else {
        l_error("mutex initialization for %p has failed", mutex);
    }

    PTHR_UNLOCK
    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
// Same check-then-act race as `_mutex_t_static_init()` above -- see that comment.
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == cond) {
            PTHR_UNLOCK
            return 0;
        }
    }

    pthread_cond_t c;
    cond->real_ptr = malloc(sizeof(pthread_cond_t));
    sceClibMemcpy(cond->real_ptr, &c, sizeof(pthread_cond_t));

    ret = pthread_cond_init(cond->real_ptr, attr);

    if (ret == 0) {
        for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
            if (initializedObjects[i] == 0) {
                initializedObjects[i] = cond;
                break;
            }
        }
        l_checkpoint(12, "pthr: first-time lazy cond init %p from thread 0x%x", cond, sceKernelGetThreadId());
    } else {
        l_error("cond initialization for %p has failed", cond);
    }

    PTHR_UNLOCK
    return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    // pthread_mutex_init_soloader was the one _soloader entry point never
    // checkpointed -- [011] inside _mutex_t_static_init only logs on
    // success, and only the bare mutex address, not `attr` (needed to spot
    // an explicit pthread_mutex_init(&obj, &recursive_attr) call, e.g. the
    // game's own glf::Mutex::Impl constructor -- see port_progress.md for
    // the 0x98673b88-signature crash this is chasing).
    l_checkpoint(21, "pthr: init mutex=%p attr=%p from thread 0x%x",
                 uid, attr, sceKernelGetThreadId());
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    forgetObject(mutex);
    // [013]/[014]/[015] are temporary triage instrumentation for the
    // 0x98673b88-signature crash in PSVGMV002 (pthread_mutex_unlock jumping
    // to a garbage PC) -- confirmed bit-identical across 4 separate real-
    // hardware runs. If that real_ptr is ever destroyed and its chunk gets
    // reused by an unrelated allocation before every stale reference to it
    // is gone, a later lock/unlock on the stale bionic mutex would explain
    // the corruption without needing any thread-scheduling race. Remove once
    // the real cause is confirmed -- see port_progress.md.
    l_checkpoint(15, "pthr: destroy mutex=%p real_ptr=%p from thread 0x%x",
                 mutex, mutex->real_ptr, sceKernelGetThreadId());
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    // *real_ptr is logged too (not just the real_ptr slot's own address) --
    // confirmed via checkpoint [016] that the crash's dereferenced value
    // (0x98673b88) falls INSIDE the .so's own data range, not our heap, so
    // the working theory is real_ptr's 4-byte slot itself gets clobbered
    // with a .so-data-looking value instead of holding the calloc'd PTE
    // struct pointer pthread_mutex_init() wrote there. See port_progress.md.
    l_checkpoint(13, "pthr: lock mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
                 mutex, mutex->real_ptr,
                 mutex->real_ptr ? *(void **) mutex->real_ptr : NULL,
                 sceKernelGetThreadId());
    return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    // lock_soloader/trylock_soloader both lazily convert a raw Bionic static
    // initializer (0 = normal, but ALSO 0x4000 = recursive, 0x8000 =
    // errorcheck -- see BIONIC_PTHREAD_*_MUTEX_INITIALIZER above) into a real
    // heap-allocated handle via _mutex_t_static_init() before touching
    // real_ptr. This entry point used to skip that call and only check
    // `!mutex->real_ptr` -- which catches the *normal* sentinel (0) but NOT
    // 0x4000/0x8000, which are non-NULL. A mutex whose very first operation
    // is unlock() (or one initialized with a non-default attr the raw-value
    // heuristic can't see) would reach the real pthread_mutex_unlock() with
    // real_ptr still holding that raw magic number instead of a valid
    // pthread_mutex_t handle, which pthread_mutex_unlock() then dereferences
    // as if it were one -- same crash signature confirmed twice in thread
    // PSVGMV002 (gangstarmiamivindication-psp2core-1788241153 and
    // -1788268266): Data abort inside the real pthread_mutex_unlock() with a
    // garbage jump target, byte-identical across separate runs (consistent
    // with a deterministic bad-handle dereference, not scheduling-dependent
    // timing). _mutex_t_static_init() is a no-op if already initialized, so
    // this is always safe to call here too.
    _mutex_t_static_init(mutex, NULL);
    if (!mutex->real_ptr) return EINVAL;
    l_checkpoint(14, "pthr: unlock mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
                 mutex, mutex->real_ptr, *(void **) mutex->real_ptr,
                 sceKernelGetThreadId());
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    // The real pthread_cond_timedwait() internally unlocks/relocks `mutex`
    // ITSELF (bypassing pthread_mutex_lock_soloader/unlock_soloader and their
    // [013]/[014] checkpoints entirely) -- if the 0x98673b88-signature crash
    // (see port_progress.md) is happening through this path instead, this is
    // the last point where mutex/real_ptr/*real_ptr can be logged before
    // control leaves our code.
    l_checkpoint(18, "pthr: cond_timedwait cond=%p mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
                 cond, mutex, mutex->real_ptr,
                 mutex->real_ptr ? *(void **) mutex->real_ptr : NULL,
                 sceKernelGetThreadId());
    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    // Same rationale as pthread_cond_timedwait_soloader above.
    l_checkpoint(17, "pthr: cond_wait cond=%p mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
                 cond, mutex, mutex->real_ptr,
                 mutex->real_ptr ? *(void **) mutex->real_ptr : NULL,
                 sceKernelGetThreadId());
    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
