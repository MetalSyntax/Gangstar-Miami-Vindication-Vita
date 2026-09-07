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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
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

// Per-mutex-operation tracing ([011]/[012]/[013]/[014]/[015]/[021]).
//
// This was the instrumentation that found the PSVGMV002 real_ptr race (see
// port_progress.md Fase 4), but it must stay OFF by default: the game
// statically links its own dlmalloc, so pthread_mutex_lock/unlock run once per
// malloc() and once per free(), and every l_checkpoint() line costs an
// sceIoOpen/sceIoWrite/sceIoClose round-trip on ux0:. That is what turned the
// engine's resource-loading path into the apparent infinite black-screen loop
// in logs/debug_local_013.log: 2600 log lines of nothing but
// `lock/unlock mutex=0x98aabcb8` -- which the .so's own symbols identify as
// `_gm_`'s dlmalloc lock, i.e. ordinary allocation traffic, not a deadlock.
// Rebuild with -DPTHR_TRACE_LOCKS to get it back for the next mutex bug.
#ifdef PTHR_TRACE_LOCKS
#define PTHR_TRACE(n, fmt, ...) l_checkpoint(n, fmt, ##__VA_ARGS__)
#else
#define PTHR_TRACE(n, fmt, ...) do {} while (0)
#endif

void * initializedObjects[PTHR_MAX_OBJECTS] = {0};
static SceKernelLwMutexWork pthr_mutex;

// Direct-mapped "already initialized" cache in front of `initializedObjects`.
//
// `_mutex_t_static_init()` runs on EVERY pthread_mutex_lock/unlock, and this
// game statically links its own dlmalloc (`_gm_` at .bss+0xaabb00, its lock at
// +0xaabcb8 -- the single mutex that dominates logs/debug_local_013.log), so
// that path is taken once per malloc() and once per free(). Doing it the old
// way -- take the global PTHR_LOCK, then linearly scan 1024 slots -- serialized
// and taxed every allocation the engine makes while loading. The cache answers
// the overwhelmingly common "yes, this one is already set up" case with a
// single load and no locking; a miss falls through to the exact, unchanged
// slow path below, so the cache can only ever be an optimization.
//
// Entries are only ever published for objects that ARE in `initializedObjects`,
// and are cleared again by forgetObject(), so a hit is never stale unless the
// game destroys a mutex while another thread is locking it (already UB).
#define PTHR_CACHE_SIZE 1024u
#define PTHR_CACHE_MASK (PTHR_CACHE_SIZE - 1u)
static atomic_uintptr_t pthr_init_cache[PTHR_CACHE_SIZE];

PTHR_INLINE unsigned _pthr_cache_slot(const void * obj) {
    // Mutex addresses are at least 4-byte aligned and often laid out in
    // regular strides, so the low bits alone make a poor index -- mix first.
    uint32_t h = (uint32_t) (uintptr_t) obj;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h & PTHR_CACHE_MASK;
}

PTHR_INLINE int _pthr_cache_hit(const void * obj) {
    return atomic_load_explicit(&pthr_init_cache[_pthr_cache_slot(obj)],
                                memory_order_acquire) == (uintptr_t) obj;
}

// Must only be called for an object already present in `initializedObjects`,
// and only once its real_ptr holds a fully initialized handle.
PTHR_INLINE void _pthr_cache_put(const void * obj) {
    atomic_store_explicit(&pthr_init_cache[_pthr_cache_slot(obj)],
                          (uintptr_t) obj, memory_order_release);
}

PTHR_INLINE void _pthr_cache_drop(const void * obj) {
    unsigned slot = _pthr_cache_slot(obj);
    uintptr_t expected = (uintptr_t) obj;
    atomic_compare_exchange_strong_explicit(&pthr_init_cache[slot], &expected, 0,
                                            memory_order_release,
                                            memory_order_relaxed);
}

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
    // Drop the fast-path entry first: after this point a concurrent
    // _mutex_t_static_init() can only reach the exact, locked slow path.
    _pthr_cache_drop(mut);
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

    // Hot path: this runs once per malloc() and once per free() (the game's
    // statically linked dlmalloc guards `_gm_` with a bionic mutex), so the
    // already-initialized case must cost a single load. See the cache's
    // comment above -- a miss just falls through to the same exact check.
    if (_pthr_cache_hit(mutex)) return 0;

    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mutex) {
            _pthr_cache_put(mutex);
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

    pthread_mutex_t * real = malloc(sizeof(pthread_mutex_t));
    if (!real) {
        l_error("mutex allocation for %p has failed", mutex);
        PTHR_UNLOCK
        return ENOMEM;
    }
    sceClibMemset(real, 0, sizeof(pthread_mutex_t));

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(real, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        // Publish the finished handle BEFORE anything can advertise this mutex
        // as initialized: the fast-path cache is read without holding
        // PTHR_LOCK, so a reader that sees the cache entry must be guaranteed
        // to see a real_ptr pointing at an already-pthread_mutex_init()'d
        // object -- not the half-built one the old ordering exposed by
        // assigning mutex->real_ptr before calling pthread_mutex_init() on it.
        mutex->real_ptr = real;
        for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
            if (initializedObjects[i] == 0) {
                initializedObjects[i] = mutex;
                break;
            }
        }
        _pthr_cache_put(mutex);
        PTHR_TRACE(11, "pthr: first-time lazy mutex init %p from thread 0x%x", mutex, sceKernelGetThreadId());
    } else {
        free(real);
        l_error("mutex initialization for %p has failed", mutex);
    }

    PTHR_UNLOCK
    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
// Same check-then-act race as `_mutex_t_static_init()` above -- see that comment.
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    if (_pthr_cache_hit(cond)) return 0;

    PTHR_LOCK
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == cond) {
            _pthr_cache_put(cond);
            PTHR_UNLOCK
            return 0;
        }
    }

    pthread_cond_t * real = malloc(sizeof(pthread_cond_t));
    if (!real) {
        l_error("cond allocation for %p has failed", cond);
        PTHR_UNLOCK
        return ENOMEM;
    }
    sceClibMemset(real, 0, sizeof(pthread_cond_t));

    ret = pthread_cond_init(real, attr);

    if (ret == 0) {
        // Same publish-after-init ordering as _mutex_t_static_init(); see there.
        cond->real_ptr = real;
        for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
            if (initializedObjects[i] == 0) {
                initializedObjects[i] = cond;
                break;
            }
        }
        _pthr_cache_put(cond);
        PTHR_TRACE(12, "pthr: first-time lazy cond init %p from thread 0x%x", cond, sceKernelGetThreadId());
    } else {
        free(real);
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
    PTHR_TRACE(21, "pthr: init mutex=%p attr=%p from thread 0x%x",
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
    PTHR_TRACE(15, "pthr: destroy mutex=%p real_ptr=%p from thread 0x%x",
               mutex, mutex->real_ptr, sceKernelGetThreadId());
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

// Liveness probe replacing the old per-lock [013] trace.
//
// It answers the only question the flood of lock/unlock lines was actually
// answering -- "is the .so still making progress, and how fast?" -- for a
// bounded cost: the clock is read once every 4096 locks, and at most one line
// is logged every PTHR_PROGRESS_INTERVAL_US. A stalled engine now shows up as
// the counter standing still between two [023] lines instead of as an
// unbounded wall of text that itself causes the stall.
#ifdef DEBUG_SOLOADER
#define PTHR_PROGRESS_INTERVAL_US (3 * 1000 * 1000)
#define PTHR_PROGRESS_SAMPLE_MASK 0xFFFu
static atomic_ulong pthr_lock_count = 0;
static SceUInt64 pthr_progress_last_us = 0;

PTHR_INLINE void _pthr_note_lock(const void * mutex, void * caller) {
    unsigned long n = atomic_fetch_add_explicit(&pthr_lock_count, 1,
                                                memory_order_relaxed) + 1;
    if ((n & PTHR_PROGRESS_SAMPLE_MASK) != 0) return;

    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - pthr_progress_last_us < PTHR_PROGRESS_INTERVAL_US) return;
    pthr_progress_last_us = now;
    l_checkpoint(23, "pthr: %lu mutex locks so far (last mutex=%p caller=%p)",
                 n, mutex, caller);
}
#else
#define _pthr_note_lock(mutex, caller) do {} while (0)
#endif

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    _pthr_note_lock(mutex, __builtin_return_address(0));
    // *real_ptr is traced too (not just the real_ptr slot's own address) --
    // confirmed via checkpoint [016] that the 0x98673b88 crash's dereferenced
    // value falls INSIDE the .so's own data range, not our heap. Only under
    // -DPTHR_TRACE_LOCKS; see PTHR_TRACE above for why it can't be on by
    // default. See port_progress.md.
    PTHR_TRACE(13, "pthr: lock mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
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
    PTHR_TRACE(14, "pthr: unlock mutex=%p real_ptr=%p *real_ptr=%p from thread 0x%x",
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
