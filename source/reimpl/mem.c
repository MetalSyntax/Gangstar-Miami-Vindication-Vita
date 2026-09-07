/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

// Anonymous mmap is emulated with malloc(), so munmap() must never hand free()
// anything that is not exactly a pointer malloc() returned here. That is not a
// theoretical concern for this game: it statically links its own dlmalloc,
// which calls CALL_MUNMAP() from sys_trim() to release only the TAIL of a
// segment -- `CALL_MUNMAP(sp->base + newsize, extra)`, an interior pointer.
// free()ing that corrupts the newlib heap in a way that surfaces much later,
// somewhere unrelated. So keep the base/length of every emulated mapping and
// only free on an exact match; report failure for anything else, which
// dlmalloc handles by simply keeping the segment it wanted to trim.
#define MMAP_MAX_REGIONS 256
static void  *mmap_base[MMAP_MAX_REGIONS];
static size_t mmap_len[MMAP_MAX_REGIONS];

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; ++i) {
        if (!mmap_base[i]) { slot = i; break; }
    }
    if (slot < 0) {
        l_error("mmap: out of bookkeeping slots (%i live mappings)", MMAP_MAX_REGIONS);
        return MAP_FAILED;
    }

    void *ret = malloc(length);
    if (!ret) {
        l_error("mmap: malloc(%i) failed", length);
        return MAP_FAILED;
    }
    memset(ret, 0, length);

    mmap_base[slot] = ret;
    mmap_len[slot] = length;
    return ret;
}

int munmap(void *addr, size_t length) {
    if (!addr) return 0;

    for (int i = 0; i < MMAP_MAX_REGIONS; ++i) {
        if (mmap_base[i] == addr) {
            if (length < mmap_len[i]) {
                // Partial unmap of the head of a mapping: malloc() has no way
                // to shrink in place, so refuse instead of freeing the whole
                // block out from under the still-mapped remainder.
                l_warn("munmap(%p, %i): partial unmap of a %i-byte mapping, ignored",
                       addr, length, mmap_len[i]);
                return -1;
            }
            mmap_base[i] = NULL;
            mmap_len[i] = 0;
            free(addr);
            return 0;
        }
    }

    l_warn("munmap(%p, %i): not a base address of any live mapping, ignored",
           addr, length);
    return -1;
}

#define SBRK_HEAP_SIZE (32 * 1024 * 1024)
static uint8_t *sbrk_heap = NULL;
static size_t sbrk_used = 0;

void *sbrk_soloader(intptr_t increment) {
    if (!sbrk_heap) {
        sbrk_heap = memalign(4096, SBRK_HEAP_SIZE);
        if (!sbrk_heap) {
            l_error("sbrk_soloader: failed to allocate %i bytes", SBRK_HEAP_SIZE);
            return (void *)-1;
        }
    }
    if (increment == 0) {
        return sbrk_heap + sbrk_used;
    }
    if (sbrk_used + increment > SBRK_HEAP_SIZE || (intptr_t)sbrk_used + increment < 0) {
        l_error("sbrk_soloader: out of memory (used: %i, increment: %i)", sbrk_used, increment);
        return (void *)-1;
    }
    void *ret = sbrk_heap + sbrk_used;
    sbrk_used += increment;
    return ret;
}
