/*
 * filecache.h -- In-memory directory & file existence cache for PS Vita
 *
 * Based on best practices from Rinnegatamante (UT99-Vita) and TheFlow (gtasa_vita).
 * Eliminates thousands of blocking filesystem scans (sceIoGetstat / sceLibcBridge_fopen)
 * for missing/existing game assets on FAT32 storage during loading screens.
 */

#ifndef GMV_FILECACHE_H
#define GMV_FILECACHE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scans the game's data directory and indexes all files in memory.
 * Must be called at boot before game assets are queried.
 */
void filecache_init(void);

/**
 * Checks if a file exists in the cache (O(1) lookup).
 * @param relpath  Relative path from data directory (e.g. "miami.bdae", "atlas0.tga")
 * @return 1 if found, 0 if not found.
 */
int filecache_exists(const char *relpath);

/**
 * Looks up a file in the cache and retrieves its size and mode if present.
 * @param relpath   Relative path from data directory
 * @param out_size  Receives cached file size (can be NULL)
 * @param out_mode  Receives cached file mode (can be NULL)
 * @return 1 if found, 0 if not found.
 */
int filecache_lookup(const char *relpath, uint32_t *out_size, uint16_t *out_mode);

/**
 * Inserts or updates an entry in the file cache (e.g. newly created file).
 */
void filecache_insert(const char *relpath, uint32_t size, uint16_t mode);

/**
 * Removes an entry from the file cache (e.g. unlinked file).
 */
void filecache_remove(const char *relpath);

/**
 * Returns total count of indexed files in the cache.
 */
uint32_t filecache_get_count(void);

#ifdef __cplusplus
}
#endif

#endif // GMV_FILECACHE_H
