/*
 * filecache.c -- In-memory directory & file existence cache for PS Vita
 *
 * Based on best practices from Rinnegatamante (UT99-Vita) and TheFlow (gtasa_vita).
 * Reduces loading times across startup and match start by caching directory
 * listings and eliminating thousands of slow flash memory linear scans.
 */

#include "utils/filecache.h"
#include "utils/logger.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FC_TABLE_SIZE 16384 // Power of 2, load factor ~20% for 3,300 files
#define FC_MAX_NAME_LEN 72

typedef struct {
    uint32_t hash;
    uint32_t size;
    uint16_t mode;
    uint16_t in_use;
    char name[FC_MAX_NAME_LEN];
} file_cache_entry_t;

static file_cache_entry_t *s_table = NULL;
static uint32_t s_count = 0;
static SceKernelLwMutexWork s_fc_mutex;
static int s_mutex_initialized = 0;

static inline void _fc_lock(void) {
    if (s_mutex_initialized)
        sceKernelLockLwMutex(&s_fc_mutex, 1, NULL);
}

static inline void _fc_unlock(void) {
    if (s_mutex_initialized)
        sceKernelUnlockLwMutex(&s_fc_mutex, 1);
}

static inline uint32_t _fc_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '\\') c = '/';
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h ? h : 1; // 0 reserved for empty
}

static inline int _fc_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca != cb) return 0;
    }
    return (*a == '\0' && *b == '\0');
}

static const char * _fc_sanitize_relpath(const char *relpath) {
    if (!relpath) return NULL;
    while (*relpath == '/') relpath++;
    while (relpath[0] == '.' && relpath[1] == '/') {
        relpath += 2;
        while (*relpath == '/') relpath++;
    }
    return relpath;
}

void filecache_insert(const char *relpath, uint32_t size, uint16_t mode) {
    if (!s_table || !relpath) return;
    relpath = _fc_sanitize_relpath(relpath);
    if (!*relpath) return;

    uint32_t h = _fc_hash(relpath);
    uint32_t idx = h & (FC_TABLE_SIZE - 1);
    uint32_t start = idx;

    _fc_lock();
    while (s_table[idx].in_use) {
        if (s_table[idx].hash == h && _fc_name_eq(s_table[idx].name, relpath)) {
            // Update existing entry
            s_table[idx].size = size;
            s_table[idx].mode = mode;
            _fc_unlock();
            return;
        }
        idx = (idx + 1) & (FC_TABLE_SIZE - 1);
        if (idx == start) {
            _fc_unlock();
            l_warn("filecache: hash table full! (size=%d)", FC_TABLE_SIZE);
            return;
        }
    }

    s_table[idx].hash = h;
    s_table[idx].size = size;
    s_table[idx].mode = mode;
    s_table[idx].in_use = 1;
    strncpy(s_table[idx].name, relpath, sizeof(s_table[idx].name) - 1);
    s_table[idx].name[sizeof(s_table[idx].name) - 1] = '\0';
    s_count++;
    _fc_unlock();
}

void filecache_remove(const char *relpath) {
    if (!s_table || !relpath) return;
    relpath = _fc_sanitize_relpath(relpath);
    if (!*relpath) return;

    uint32_t h = _fc_hash(relpath);
    uint32_t idx = h & (FC_TABLE_SIZE - 1);
    uint32_t start = idx;

    _fc_lock();
    while (s_table[idx].in_use) {
        if (s_table[idx].hash == h && _fc_name_eq(s_table[idx].name, relpath)) {
            s_table[idx].hash = 0;
            s_table[idx].in_use = 0;
            s_table[idx].name[0] = '\0';
            if (s_count > 0) s_count--;
            _fc_unlock();
            return;
        }
        idx = (idx + 1) & (FC_TABLE_SIZE - 1);
        if (idx == start) break;
    }
    _fc_unlock();
}

int filecache_lookup(const char *relpath, uint32_t *out_size, uint16_t *out_mode) {
    if (!s_table || !relpath) return 0;
    relpath = _fc_sanitize_relpath(relpath);
    if (!*relpath) return 0;

    uint32_t h = _fc_hash(relpath);
    uint32_t idx = h & (FC_TABLE_SIZE - 1);
    uint32_t start = idx;

    _fc_lock();
    while (s_table[idx].in_use) {
        if (s_table[idx].hash == h && _fc_name_eq(s_table[idx].name, relpath)) {
            if (out_size) *out_size = s_table[idx].size;
            if (out_mode) *out_mode = s_table[idx].mode;
            _fc_unlock();
            return 1;
        }
        idx = (idx + 1) & (FC_TABLE_SIZE - 1);
        if (idx == start) break;
    }
    _fc_unlock();
    return 0;
}

int filecache_exists(const char *relpath) {
    return filecache_lookup(relpath, NULL, NULL);
}

uint32_t filecache_get_count(void) {
    return s_count;
}

static void _scan_directory_recursive(const char *dir_path, const char *rel_prefix) {
    SceUID dfd = sceIoDopen(dir_path);
    if (dfd < 0) {
        l_warn("filecache: could not open directory %s (err: 0x%08X)", dir_path, dfd);
        return;
    }

    SceIoDirent dirent;
    sceClibMemset(&dirent, 0, sizeof(SceIoDirent));

    while (sceIoDread(dfd, &dirent) > 0) {
        if (dirent.d_name[0] == '.')
            continue; // Skip "." and ".." and hidden files

        char sub_rel[128];
        if (rel_prefix && rel_prefix[0]) {
            snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_prefix, dirent.d_name);
        } else {
            snprintf(sub_rel, sizeof(sub_rel), "%s", dirent.d_name);
        }

        if (SCE_S_ISDIR(dirent.d_stat.st_mode)) {
            char sub_full[512];
            snprintf(sub_full, sizeof(sub_full), "%s/%s", dir_path, dirent.d_name);
            _scan_directory_recursive(sub_full, sub_rel);
        } else {
            filecache_insert(sub_rel, (uint32_t)dirent.d_stat.st_size, (uint16_t)dirent.d_stat.st_mode);
        }
        sceClibMemset(&dirent, 0, sizeof(SceIoDirent));
    }

    sceIoDclose(dfd);
}

void filecache_init(void) {
    if (s_table) return; // Already initialized

    if (!s_mutex_initialized) {
        sceKernelCreateLwMutex(&s_fc_mutex, "filecache_mutex", 0, 0, NULL);
        s_mutex_initialized = 1;
    }

    s_table = (file_cache_entry_t *)calloc(FC_TABLE_SIZE, sizeof(file_cache_entry_t));
    if (!s_table) {
        l_fatal("filecache: could not allocate memory for file cache table!");
        return;
    }

    uint64_t t0 = sceKernelGetProcessTimeWide();
    const char *data_dir = DATA_PATH "data";
    _scan_directory_recursive(data_dir, "");
    uint64_t t1 = sceKernelGetProcessTimeWide();

    l_success("filecache: indexed %u files from %s in %u us (table size: %d)",
              (unsigned)s_count, data_dir, (unsigned)(t1 - t0), FC_TABLE_SIZE);
}
