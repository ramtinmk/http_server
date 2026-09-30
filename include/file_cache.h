#ifndef FILE_CACHE_H
#define FILE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/*
 * One cached or streamed representation of a resource.
 *
 * A representation is either memory-backed (`body != NULL`, owned by the cache
 * or by the response for heap-owned multipart bodies) or file-backed
 * (`body == NULL`, `fd >= 0`): the latter is never cached and is streamed by
 * the event loop with sendfile(). `fd` in a cached representation is unused.
 */
typedef struct {
    unsigned char *body;      /* Cached bytes, or NULL when fd-backed. */
    size_t         body_len;  /* Representation length in bytes. */
    int            fd;        /* Streamed file descriptor, or -1. */
    off_t          file_off;  /* Starting offset for the streamed body. */
    time_t         mtime;
    ino_t          inode;
    char           etag[64];
    char           last_modified[40];
} Representation;

typedef struct FileCache FileCache;
struct CacheEntry;

/*
 * Create a cache with a total byte budget and an entry cap. A budget of 0
 * disables admission (every lookup misses). Returns NULL on allocation
 * failure.
 */
FileCache *file_cache_create(size_t budget_bytes, size_t max_entries);

/* Free the cache and every entry it owns. Must not run with live references. */
void file_cache_destroy(FileCache *cache);

/* Lower the budget, evicting unreferenced LRU entries as needed. */
void file_cache_set_budget(FileCache *cache, size_t budget_bytes);

size_t file_cache_bytes(const FileCache *cache);
size_t file_cache_entry_count(const FileCache *cache);

/*
 * Look up `key` and, on a hit, return an entry with an extra reference. The
 * caller must release it with file_cache_release(). Returns NULL on a miss.
 */
struct CacheEntry *file_cache_acquire(FileCache *cache, const char *key);

/* Representation accessors; valid while the entry is referenced. */
const Representation *file_cache_plain(const struct CacheEntry *entry);
const Representation *file_cache_gzip(const struct CacheEntry *entry);

/* Drop a reference, freeing the entry when it is unreferenced and evicted. */
void file_cache_release(struct CacheEntry *entry);

/*
 * Admit a representation pair. On success the cache takes ownership of the
 * body pointers in `plain`/`gzip` (they must be heap-allocated) and returns a
 * new entry with one reference held by the caller. Returns NULL when the pair
 * does not fit the current budget, the entry cap is reached with every entry
 * pinned, or allocation fails; on failure the caller keeps ownership of the
 * bodies and must free them.
 */
struct CacheEntry *file_cache_insert(FileCache *cache, const char *key,
                                     const Representation *plain,
                                     const Representation *gzip);

#endif /* FILE_CACHE_H */
