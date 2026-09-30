#ifndef PATH_RESOLVER_H
#define PATH_RESOLVER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/*
 * Safe document-root path resolution (Phase 2).
 *
 * A request-target path is percent-decoded exactly once, normalized, and
 * opened relative to the configured document root. Resolution never escapes
 * the root: `..` segments that would climb above it are refused, hidden
 * (leading-dot) segments are refused unless allowed, and symlink traversal is
 * confined to the root (and can be forbidden outright). Directories resolve to
 * one of the configured index files; there is no directory listing.
 */

typedef enum {
    PATH_OK = 0,          /* resolved to a regular file; fd is open */
    PATH_ERR_MALFORMED,   /* bad escape/backslash/control -> 400 */
    PATH_ERR_TOO_LONG,    /* over-long encoded path -> 414 */
    PATH_ERR_FORBIDDEN,   /* hidden/symlink/escape policy -> 403 */
    PATH_ERR_NOT_FOUND,   /* no such file, or directory with no index -> 404 */
    PATH_ERR_UNAVAILABLE  /* unexpected I/O error, or root not configured */
} PathStatus;

typedef struct {
    int    fd;             /* Open O_RDONLY fd for the regular file, or -1. */
    off_t  size;           /* File size in bytes. */
    time_t mtime;          /* st_mtime. */
    ino_t  inode;          /* st_ino. */
    dev_t  device;         /* st_dev. */
    char   path[1024];     /* Normalized path relative to the root. */
} ResolvedFile;

/*
 * Initialize the resolver. Opens and validates `root` as a directory, stores
 * the index-file names, and (when non-empty) loads `mime_file` as a
 * mime.types-style map extending the builtin table. Returns 0 on success, -1
 * on a missing/unreadable root or a malformed MIME file.
 */
int path_resolver_init(const char *root, const char *index_files,
                       const char *mime_file, int hidden_allowed,
                       int symlinks_allowed);

/* Release resolver-owned resources (the root descriptor and MIME table). */
void path_resolver_shutdown(void);

/*
 * Resolve `path` (the raw routing path, query already stripped) beneath the
 * document root. On PATH_OK, `out` holds an open regular-file descriptor that
 * the caller owns. On any other status `out->fd` is -1 and no descriptor is
 * leaked.
 */
PathStatus path_resolver_open(const char *path, ResolvedFile *out);

/* Close a descriptor returned by path_resolver_open (a no-op when fd < 0). */
void path_resolver_close(ResolvedFile *file);

/* MIME type for `path` by extension, or "application/octet-stream". */
const char *mime_type_for_path(const char *path);

#endif /* PATH_RESOLVER_H */
