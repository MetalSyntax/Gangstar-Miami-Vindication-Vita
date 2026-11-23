/*
 * Fase 74: RAM-backed read-only FILEs, streamed in by a reader thread on
 * core 1.
 *
 * The engine parses its .bdae/.bsprite/.tga assets with thousands of tiny
 * fseek()+fread() pairs (io.c, Fase 16 trace). Through SceLibc each seek
 * throws away the 128 KB stream buffer, so every pair is another memory-card
 * read, all serial on the render thread (core 0): the 13-15 s loads of
 * log 073. Here a read-only asset up to MEMFILE_MAX is handed to a worker on
 * core 1 that reads it front to back in big chunks, while core 0 already
 * parses whatever has arrived; every FILE call on it becomes a memcpy. A call
 * only waits when it asks for bytes the worker has not delivered yet.
 *
 * Dispatch: dynlib.c points the engine's FILE imports at the mf_* functions;
 * anything that is not one of our handles (registry lookup) goes to
 * SceLibcBridge unchanged.
 */

#include "reimpl/memfile.h"
#include "utils/logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <libc_bridge/libc_bridge.h>

#define MEMFILE_MAX   (4 * 1024 * 1024)  /* bigger files (miami.bdae, fonts) stay native */
#define MEMFILE_CHUNK (256 * 1024)
#define MEMFILE_SLOTS 64                  /* live handles; beyond this, native */
#define MEMFILE_MAGIC 0x4D454D46u

typedef struct memfile {
    uint32_t magic;
    uint8_t *buf;
    uint32_t size;
    uint32_t pos;
    volatile uint32_t loaded;  /* bytes the worker has delivered */
    volatile int done;         /* worker finished (ok or failed) */
    volatile int cancel;       /* fclose before the worker finished */
    int failed, eof;
    struct memfile *next;      /* worker queue */
    char path[256];
} memfile_t;

static memfile_t *s_live[MEMFILE_SLOTS];
static SceUID s_lock = -1, s_sema = -1;
static memfile_t *s_head, *s_tail;
static int s_ready;

/* Stats for the slow-frame log line (main.c). */
static volatile uint32_t s_opens, s_bytes;
static volatile uint64_t s_wait_us;

static memfile_t *mf_of(FILE *f) {
    memfile_t *m = (memfile_t *)f;
    if (!m || !s_ready)
        return NULL;
    for (int i = 0; i < MEMFILE_SLOTS; i++)
        if (s_live[i] == m)
            return m->magic == MEMFILE_MAGIC ? m : NULL;
    return NULL;
}

static int mf_worker(SceSize argc, void *argv) {
    (void)argc; (void)argv;
    for (;;) {
        sceKernelWaitSema(s_sema, 1, NULL);
        sceKernelLockMutex(s_lock, 1, NULL);
        memfile_t *m = s_head;
        if (m) { s_head = m->next; if (!s_head) s_tail = NULL; }
        sceKernelUnlockMutex(s_lock, 1);
        if (!m)
            continue;

        SceUID fd = m->cancel ? -1 : sceIoOpen(m->path, SCE_O_RDONLY, 0);
        uint32_t got = 0;
        while (fd >= 0 && got < m->size && !m->cancel) {
            uint32_t want = m->size - got;
            if (want > MEMFILE_CHUNK) want = MEMFILE_CHUNK;
            int r = sceIoRead(fd, m->buf + got, want);
            if (r <= 0) break;
            got += (uint32_t)r;
            __sync_synchronize();  /* data before the counter */
            m->loaded = got;
        }
        if (fd >= 0) sceIoClose(fd);
        if (got < m->size) {
            m->failed = 1;
            m->size = got;  /* serve what exists; reads past it hit EOF */
        }
        __sync_synchronize();
        m->done = 1;
    }
    return 0;
}

void memfile_init(void) {
    s_lock = sceKernelCreateMutex("memfile", 0, 0, NULL);
    s_sema = sceKernelCreateSema("memfile_q", 0, 0, 1024, NULL);
    SceUID th = sceKernelCreateThread("memfile_reader", mf_worker, 0x80, 0x4000, 0,
                                      SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (s_lock < 0 || s_sema < 0 || th < 0) {
        l_warn("memfile: init failed -- assets stay on native FILE I/O");
        return;
    }
    sceKernelStartThread(th, 0, NULL);
    s_ready = 1;
    l_note("memfile: RAM-backed asset reads on, reader thread on core 1 (<= %d KB)", MEMFILE_MAX / 1024);
}

FILE *memfile_open(const char *path) {
    if (!s_ready)
        return NULL;
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0 || st.st_size <= 0 || st.st_size > MEMFILE_MAX)
        return NULL;
    if (strlen(path) >= sizeof(((memfile_t *)0)->path))
        return NULL;

    memfile_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->buf = malloc((size_t)st.st_size);
    if (!m->buf) { free(m); return NULL; }
    m->magic = MEMFILE_MAGIC;
    m->size = (uint32_t)st.st_size;
    strcpy(m->path, path);

    sceKernelLockMutex(s_lock, 1, NULL);
    int slot = -1;
    for (int i = 0; i < MEMFILE_SLOTS; i++)
        if (!s_live[i]) { slot = i; break; }
    if (slot < 0) {
        sceKernelUnlockMutex(s_lock, 1);
        free(m->buf); free(m);
        return NULL;
    }
    s_live[slot] = m;
    if (s_tail) s_tail->next = m; else s_head = m;
    s_tail = m;
    sceKernelUnlockMutex(s_lock, 1);
    sceKernelSignalSema(s_sema, 1);

    s_opens++;
    s_bytes += m->size;
    return (FILE *)m;
}

/* Blocks until [0, end) is in RAM or the worker is done. */
static void mf_wait(memfile_t *m, uint32_t end) {
    if (end > m->size) end = m->size;
    if (m->loaded >= end || m->done) return;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    while (m->loaded < end && !m->done)
        sceKernelDelayThread(100);
    s_wait_us += sceKernelGetProcessTimeWide() - t0;
}

void memfile_stats(uint32_t *opens, uint32_t *bytes, uint32_t *wait_ms) {
    *opens = s_opens; *bytes = s_bytes; *wait_ms = (uint32_t)(s_wait_us / 1000);
    s_opens = 0; s_bytes = 0; s_wait_us = 0;
}

/* ---- FILE API (engine imports) ---- */

size_t mf_fread(void *ptr, size_t size, size_t n, FILE *f) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_fread(ptr, size, n, f);
    if (!size || !n) return 0;
    uint32_t want = (uint32_t)(size * n);
    uint32_t avail = m->pos < m->size ? m->size - m->pos : 0;
    mf_wait(m, m->pos + (want < avail ? want : avail));
    avail = m->pos < m->size ? m->size - m->pos : 0;  /* size may shrink on failure */
    uint32_t got = want < avail ? want : avail;
    uint32_t items = got / (uint32_t)size;
    got = items * (uint32_t)size;
    if (got) sceClibMemcpy(ptr, m->buf + m->pos, got);
    m->pos += got;
    if (items < n) m->eof = 1;
    return items;
}

int mf_fseek(FILE *f, long off, int whence) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_fseek(f, off, whence);
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)m->pos
              : whence == SEEK_END ? (long)m->size : -1;
    if (base < 0 || base + off < 0) { errno = EINVAL; return -1; }
    m->pos = (uint32_t)(base + off);
    m->eof = 0;
    return 0;
}

long mf_ftell(FILE *f) {
    memfile_t *m = mf_of(f);
    return m ? (long)m->pos : sceLibcBridge_ftell(f);
}

int mf_fgetc(FILE *f) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_fgetc(f);
    mf_wait(m, m->pos + 1);
    if (m->pos >= m->size) { m->eof = 1; return EOF; }
    return m->buf[m->pos++];
}

int mf_getc(FILE *f) {
    return mf_of(f) ? mf_fgetc(f) : sceLibcBridge_getc(f);
}

int mf_ungetc(int c, FILE *f) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_ungetc(c, f);
    if (c == EOF || m->pos == 0) return EOF;
    m->buf[--m->pos] = (uint8_t)c;  /* our private copy: safe to overwrite */
    m->eof = 0;
    return (uint8_t)c;
}

int mf_feof(FILE *f) {
    memfile_t *m = mf_of(f);
    return m ? m->eof : sceLibcBridge_feof(f);
}

int mf_ferror(FILE *f) {
    memfile_t *m = mf_of(f);
    return m ? m->failed : sceLibcBridge_ferror(f);
}

int mf_fflush(FILE *f) {
    return mf_of(f) ? 0 : sceLibcBridge_fflush(f);
}

int mf_setvbuf(FILE *f, char *buf, int mode, size_t size) {
    return mf_of(f) ? 0 : sceLibcBridge_setvbuf(f, buf, mode, size);
}

/* bionic fpos_t is a 32-bit off_t. */
int mf_fgetpos(FILE *f, fpos_t *pos) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_fgetpos(f, pos);
    *(int32_t *)pos = (int32_t)m->pos;
    return 0;
}

int mf_fsetpos(FILE *f, const fpos_t *pos) {
    memfile_t *m = mf_of(f);
    if (!m) return sceLibcBridge_fsetpos(f, pos);
    return mf_fseek(f, *(const int32_t *)pos, SEEK_SET);
}

/* Returns 1 if f was ours (and is now closed), 0 if the caller must close it. */
int memfile_close(FILE *f) {
    memfile_t *m = mf_of(f);
    if (!m) return 0;
    sceKernelLockMutex(s_lock, 1, NULL);
    for (int i = 0; i < MEMFILE_SLOTS; i++)
        if (s_live[i] == m) s_live[i] = NULL;
    sceKernelUnlockMutex(s_lock, 1);
    m->cancel = 1;
    while (!m->done)  /* the worker may still be writing into buf */
        sceKernelDelayThread(100);
    m->magic = 0;
    free(m->buf);
    free(m);
    return 1;
}
