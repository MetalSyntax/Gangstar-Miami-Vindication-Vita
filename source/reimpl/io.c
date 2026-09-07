/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

// ---------------------------------------------------------------------------
// Android -> Vita path translation
// ---------------------------------------------------------------------------
//
// This .so has its content root compiled in as a literal (`strings` on
// libGangster2.so: "/sdcard/gameloft/games/Gangstar2/" at .rodata+0x5e0d60,
// plus ".../igp" and ".../tmp/", and the app-private Android dir
// "/data/data/com.gameloft.android.TBFV.GloftGMHP.ML/"). There is no JNI entry
// point that sets it -- the only Java_* symbols the library exports are the
// five nativeInit/nativeRender ones main.c already calls -- so the paths can
// only be fixed here, on the way into the filesystem.
//
// Without this, `glitch::io::createReadFile()` returns NULL for every asset and
// `ASprite::ASprite(const char*)` (0x364b28) dereferences that NULL without
// checking, at +0x96: `blx r3` (the vfs open virtual) then `ldr r3, [r0]` with
// r0 == 0. That is the Data abort in
// gangstarmiamivindication-psp2core-1788583236 (R0 = R4 = 0, the stack holding
// the ASCII of "./huds_hi.bsprite"), and it is why logs/debug_local_014.log is
// a wall of `fopen(./sdcard/gameloft/games/Gangstar2//./huds.bsprite): 0x0`.
//
// The engine also concatenates its root with names that already start with
// "./" and sometimes prefixes the whole thing with another ".", so the paths
// that actually reach fopen() look like
// "./sdcard/gameloft/games/Gangstar2//./about.english". Normalizing the
// duplicate slashes and the "." segments is part of the job, not cosmetic:
// sceLibcBridge does not resolve them.
#define ANDROID_DATA_ROOT "/sdcard/gameloft/games/Gangstar2"
#define ANDROID_PRIV_ROOT "/data/data/com.gameloft.android.TBFV.GloftGMHP.ML"
#define VITA_DATA_ROOT    DATA_PATH "data"
#define VITA_SAVE_ROOT    DATA_PATH "saves"

#define IO_PATH_BUF 1024

// Copies `prefix` then `rest`, collapsing runs of '/' and dropping "." path
// segments ("//./" -> "/", "/./" -> "/", a trailing "/." -> "/").
static void _path_clean_copy(char * dst, size_t cap,
                             const char * prefix, const char * rest) {
    size_t len = 0;
    for (const char * p = prefix; *p && len + 1 < cap; ++p)
        dst[len++] = *p;

    const char * s = rest;
    while (*s && len + 1 < cap) {
        if (*s != '/') {
            dst[len++] = *s++;
            continue;
        }
        while (*s == '/' || (s[0] == '.' && (s[1] == '/' || s[1] == '\0'))) {
            if (*s == '/')       ++s;
            else if (s[1] == '/') s += 2;
            else                  ++s;
        }
        if (len == 0 || dst[len - 1] != '/')
            dst[len++] = '/';
    }
    dst[len] = '\0';
}

// Creates every missing parent directory of `path`. On Android the engine can
// count on its "home" dir (/data/data/<pkg>/) and its tmp/ dir already
// existing; here they only exist if we make them, and a write that fails
// because of a missing directory is invisible until the engine trips over the
// half-written result much later.
static void _mkdir_parents(const char * path) {
    char tmp[IO_PATH_BUF];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);

    // Never mkdir the "ux0:" device prefix itself.
    char * p = strchr(tmp, ':');
    p = p ? p + 1 : tmp;

    for (; *p; ++p) {
        if (*p != '/' || p == tmp) continue;
        *p = '\0';
        if (tmp[0]) mkdir(tmp, 0777);
        *p = '/';
    }
}

// Returns either `path` unchanged (not one of ours: ux0:/app0:, /proc/..., a
// genuinely relative name) or `buf`, filled with the translated path.
static const char * _path_translate(const char * path, char * buf, size_t cap) {
    if (!path || !*path) return path;

    const char * p = path;

    // "./sdcard/..." -- only strip the leading '.' when what follows really is
    // one of the absolute Android roots; a genuine relative "./foo" must stay
    // relative.
    if (p[0] == '.' && p[1] == '/') {
        if (strncmp(p + 1, ANDROID_DATA_ROOT, sizeof(ANDROID_DATA_ROOT) - 1) == 0 ||
            strncmp(p + 1, ANDROID_PRIV_ROOT, sizeof(ANDROID_PRIV_ROOT) - 1) == 0 ||
            strncmp(p + 1, "/sdcard/", 8) == 0)
            p += 1;
    }

    const char * rest;
    const char * repl;

    if (strncmp(p, ANDROID_DATA_ROOT, sizeof(ANDROID_DATA_ROOT) - 1) == 0 &&
        (p[sizeof(ANDROID_DATA_ROOT) - 1] == '/' ||
         p[sizeof(ANDROID_DATA_ROOT) - 1] == '\0')) {
        rest = p + sizeof(ANDROID_DATA_ROOT) - 1;
        repl = VITA_DATA_ROOT;
    } else if (strncmp(p, ANDROID_PRIV_ROOT, sizeof(ANDROID_PRIV_ROOT) - 1) == 0 &&
               (p[sizeof(ANDROID_PRIV_ROOT) - 1] == '/' ||
                p[sizeof(ANDROID_PRIV_ROOT) - 1] == '\0')) {
        rest = p + sizeof(ANDROID_PRIV_ROOT) - 1;
        repl = VITA_SAVE_ROOT;
    } else if (strncmp(p, "/sdcard", 7) == 0 &&
               (p[7] == '/' || p[7] == '\0')) {
        // Anything else the engine or the IGP/ads code puts on the "SD card".
        rest = p + 7;
        repl = VITA_DATA_ROOT;
    } else {
        return path;
    }

    _path_clean_copy(buf, cap, repl, rest);
    return buf;
}


FILE * fopen_soloader(const char * filename, const char * mode) {
    if (!filename) return NULL;
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    char buf[IO_PATH_BUF];
    const char * path = _path_translate(filename, buf, sizeof(buf));

    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')))
        _mkdir_parents(path);

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(path, mode);
#else
    FILE* ret = fopen(path, mode);
#endif

    if (ret) {
        l_debug("fopen(%s, %s): %p", path, mode, ret);
    } else {
        // l_error(), not l_warn(): a missing asset is the most common cause of
        // a later NULL-deref (Fase 9) and l_warn() vanishes in a Release build.
        //
        // Dedupe (2026-09-06): the engine retries a missing file many times in
        // a row (dummy.tga x18 in debug_local_021.log), and every l_error()
        // pays for an immediate sceIoSyncByFd() to the memory card. Only the
        // FIRST miss of each path keeps l_error (one sync); repeats go through
        // l_debug (free in Release). When the path changes, one l_note line
        // reports how many repeats the previous path had, so the count is not
        // lost in Release logs.
        static char last_failed[256];
        static int last_failed_repeats = 0;
        char key[256];
        strncpy(key, filename, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        if (strcmp(key, last_failed) == 0) {
            last_failed_repeats++;
            l_debug("fopen(%s, %s): FAILED (repeat #%d) [was: %s]", path, mode,
                    last_failed_repeats + 1, filename);
        } else {
            if (last_failed_repeats > 0)
                l_note("fopen: %s failed x%d total", last_failed, last_failed_repeats + 1);
            strncpy(last_failed, key, sizeof(last_failed) - 1);
            last_failed[sizeof(last_failed) - 1] = '\0';
            last_failed_repeats = 0;
            l_error("fopen(%s, %s): FAILED [was: %s]", path, mode, filename);
        }
    }

    return ret;
}

FILE * freopen_soloader(const char * filename, const char * mode, FILE * stream) {
    char buf[IO_PATH_BUF];
    const char * path = _path_translate(filename, buf, sizeof(buf));
#ifdef USE_SCELIBC_IO
    FILE * ret = sceLibcBridge_freopen(path, mode, stream);
#else
    FILE * ret = freopen(path, mode, stream);
#endif
    l_debug("freopen(%s, %s, %p): %p", path, mode, stream, ret);
    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (!path) return -1;
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));

    if ((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT)
        _mkdir_parents(real);

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(real, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", real, oflag, ret);
    else
        l_error("open(%s, %x): FAILED [was: %s]", real, oflag, path);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    char pbuf[IO_PATH_BUF];
    const char * real = _path_translate(path, pbuf, sizeof(pbuf));

    struct stat st;
    int res = stat(real, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", real, res);
    return res;
}

// Was mapped straight to newlib's lstat() in dynlib.c, which writes a newlib
// `struct stat` into a buffer the .so sized and laid out as bionic's
// `struct stat64` -- the same mismatch stat_soloader() exists to avoid.
int lstat_soloader(const char * path, stat64_bionic * buf) {
    char pbuf[IO_PATH_BUF];
    const char * real = _path_translate(path, pbuf, sizeof(pbuf));

    struct stat st;
    int res = lstat(real, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("lstat(%s): %i", real, res);
    return res;
}

int fclose_soloader(FILE * f) {
    // TEMP triage (kernel data-abort right after DefaultEffects.bdae closes,
    // 2026-09-05): log BEFORE the call too -- if the crash is inside fclose
    // on a garbage FILE*, only the entry line will appear. Remove once found.
    l_debug("fclose(%p)...", f);
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

// Entry+exit tracing of fread/fseek, behind -DIO_TRACE_STREAMS (CMake option
// IO_TRACE_STREAMS, OFF by default).
//
// It is deliberately NOT part of a plain Debug build: the engine reads its
// .bdae/.bsprite containers with thousands of small fread()/fseek() pairs
// (debug_local_016.log is 2400 lines, ~90% of them exactly this), and the same
// mechanism that made mutex tracing fatal in Fase 8 applies here -- the log
// traffic dominates the loading path and changes the timing enough that the
// build no longer reproduces what the release build does. Turn it on only
// while chasing a specific bad read.
#ifdef IO_TRACE_STREAMS
#define IO_TRACE(...) l_debug(__VA_ARGS__)
#else
#define IO_TRACE(...)
#endif

size_t fread_soloader(void * ptr, size_t size, size_t nmemb, FILE * stream) {
    IO_TRACE("fread(%p, %u, %u, %p)...", ptr, (unsigned)size, (unsigned)nmemb, stream);
#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fread(ptr, size, nmemb, stream);
#else
    size_t ret = fread(ptr, size, nmemb, stream);
#endif
    IO_TRACE("fread(%p, %u, %u, %p): %u", ptr, (unsigned)size, (unsigned)nmemb, stream, (unsigned)ret);
    return ret;
}

int fseek_soloader(FILE * stream, long offset, int whence) {
    IO_TRACE("fseek(%p, %li, %i)...", stream, offset, whence);
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fseek(stream, offset, whence);
#else
    int ret = fseek(stream, offset, whence);
#endif
    IO_TRACE("fseek(%p, %li, %i): %i", stream, offset, whence, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(_pathname, buf, sizeof(buf));
    DIR* ret = opendir(real);
    l_debug("opendir(\"%s\"): %p", real, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

// ---------------------------------------------------------------------------
// Remaining path-taking entry points
// ---------------------------------------------------------------------------
//
// These were all mapped straight to newlib in dynlib.c, so they bypassed the
// Android -> Vita translation above and every one of them would have kept
// looking under a "/sdcard" that does not exist on this console.

int access_soloader(const char * path, int mode) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = access(real, mode);
    l_debug("access(%s, %i): %i", real, mode, ret);
    return ret;
}

int mkdir_soloader(const char * path, mode_t mode) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = mkdir(real, mode);
    l_debug("mkdir(%s, %o): %i", real, mode, ret);
    return ret;
}

int rmdir_soloader(const char * path) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = rmdir(real);
    l_debug("rmdir(%s): %i", real, ret);
    return ret;
}

int remove_soloader(const char * path) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = remove(real);
    l_debug("remove(%s): %i", real, ret);
    return ret;
}

int unlink_soloader(const char * path) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = unlink(real);
    l_debug("unlink(%s): %i", real, ret);
    return ret;
}

int rename_soloader(const char * from, const char * to) {
    char fbuf[IO_PATH_BUF], tbuf[IO_PATH_BUF];
    const char * rfrom = _path_translate(from, fbuf, sizeof(fbuf));
    const char * rto = _path_translate(to, tbuf, sizeof(tbuf));
    int ret = rename(rfrom, rto);
    l_debug("rename(%s, %s): %i", rfrom, rto, ret);
    return ret;
}

int chmod_soloader(const char * path, mode_t mode) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = chmod(real, mode);
    l_debug("chmod(%s, %o): %i", real, mode, ret);
    return ret;
}

int chdir_soloader(const char * path) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    int ret = chdir(real);
    l_debug("chdir(%s): %i", real, ret);
    return ret;
}

char * realpath_soloader(const char * path, char * resolved) {
    char buf[IO_PATH_BUF];
    const char * real = _path_translate(path, buf, sizeof(buf));
    // newlib's realpath() resolves against a cwd that means nothing here, and
    // the translated path is already absolute and normalized, so just hand it
    // back. PATH_MAX-sized `resolved` is the caller's contract.
    if (!resolved) {
        resolved = malloc(PATH_MAX);
        if (!resolved) return NULL;
    }
    strncpy(resolved, real, PATH_MAX - 1);
    resolved[PATH_MAX - 1] = '\0';
    l_debug("realpath(%s): %s", path, resolved);
    return resolved;
}

// The engine writes into ".../Gangstar2/tmp/" and the IGP code into
// ".../Gangstar2/igp"; both map under the data root, and neither is created by
// the deploy step. mkdir() on an existing directory just fails harmlessly.
void io_prepare_dirs(void) {
    mkdir(VITA_DATA_ROOT, 0777);
    mkdir(VITA_SAVE_ROOT, 0777);
    mkdir(VITA_DATA_ROOT "/tmp", 0777);
    mkdir(VITA_DATA_ROOT "/igp", 0777);
}
