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

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }
    void* ret= malloc(length);
    memset(ret, 0, length);
    return ret;
}

int munmap(void *addr, size_t length) {
    if (addr) free(addr);
    return 0;
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
