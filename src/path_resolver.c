#define _GNU_SOURCE

#include "path_resolver.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* --- openat2 (Linux 5.6+), with a portable fallback --------------------- */

#if defined(__linux__)
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS 0x02
#endif
#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS 0x04
#endif
#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x08
#endif

/* Matches the kernel layout of struct open_how. */
struct path_open_how {
    unsigned long long flags;
    unsigned long long mode;
    unsigned long long resolve;
};

static int sys_openat2(int dirfd, const char *path, unsigned long long flags,
                       unsigned long long resolve)
{
    struct path_open_how how;
    how.flags   = flags;
    how.mode    = 0;
    how.resolve = resolve;
    return (int)syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}
#endif /* __linux__ */

/* --- Module state ------------------------------------------------------- */

#define PATH_MAX_BYTES 1024
#define INDEX_MAX 8
#define INDEX_NAME_MAX 128

static int  g_root_fd = -1;
static int  g_hidden_allowed;
static int  g_symlinks_allowed;
static char g_index[INDEX_MAX][INDEX_NAME_MAX];
static size_t g_index_count;

typedef struct {
    char ext[32];
    char type[128];
} MimeEntry;

static MimeEntry *g_mime;
static size_t     g_mime_count;
static size_t     g_mime_cap;

/* --- Builtin MIME table ------------------------------------------------- */

static const struct {
    const char *ext;
    const char *type;
} BUILTIN_MIME[] = {
    { "html",  "text/html" },
    { "htm",   "text/html" },
    { "css",   "text/css" },
    { "js",    "text/javascript" },
    { "mjs",   "text/javascript" },
    { "json",  "application/json" },
    { "map",   "application/json" },
    { "txt",   "text/plain" },
    { "text",  "text/plain" },
    { "md",    "text/markdown" },
    { "csv",   "text/csv" },
    { "xml",   "application/xml" },
    { "pdf",   "application/pdf" },
    { "png",   "image/png" },
    { "jpg",   "image/jpeg" },
    { "jpeg",  "image/jpeg" },
    { "gif",   "image/gif" },
    { "webp",  "image/webp" },
    { "svg",   "image/svg+xml" },
    { "ico",   "image/x-icon" },
    { "wasm",  "application/wasm" },
    { "gz",    "application/gzip" },
    { "zip",   "application/zip" },
    { "mp4",   "video/mp4" },
    { "webm",  "video/webm" },
    { "mp3",   "audio/mpeg" },
    { "woff",  "font/woff" },
    { "woff2", "font/woff2" },
    { "ttf",   "font/ttf" },
    { "otf",   "font/otf" },
};

#define BUILTIN_MIME_COUNT (sizeof(BUILTIN_MIME) / sizeof(BUILTIN_MIME[0]))

/* --- MIME map ----------------------------------------------------------- */

static int mime_add(const char *ext, size_t ext_len, const char *type)
{
    if (ext_len == 0 || ext_len >= sizeof(g_mime[0].ext))
        return 0;
    if (g_mime_count == g_mime_cap) {
        size_t cap = g_mime_cap ? g_mime_cap * 2 : 64;
        MimeEntry *grown = realloc(g_mime, cap * sizeof(*grown));
        if (!grown)
            return -1;
        g_mime = grown;
        g_mime_cap = cap;
    }
    MimeEntry *e = &g_mime[g_mime_count++];
    for (size_t i = 0; i < ext_len; i++)
        e->ext[i] = (char)tolower((unsigned char)ext[i]);
    e->ext[ext_len] = '\0';
    snprintf(e->type, sizeof(e->type), "%s", type);
    return 0;
}

static int load_mime_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "FATAL: cannot open mime types file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *save = NULL;
        char *type = strtok_r(line, " \t\r\n", &save);
        if (!type)
            continue;
        if (strchr(type, '/') == NULL) {
            fprintf(stderr, "FATAL: %s:%d: invalid MIME type '%s'\n",
                    path, lineno, type);
            fclose(fp);
            return -1;
        }
        for (char *ext = strtok_r(NULL, " \t\r\n", &save); ext;
             ext = strtok_r(NULL, " \t\r\n", &save)) {
            if (mime_add(ext, strlen(ext), type) != 0) {
                fclose(fp);
                return -1;
            }
        }
    }
    fclose(fp);
    return 0;
}

const char *mime_type_for_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot[1] == '\0')
        return "application/octet-stream";
    const char *ext = dot + 1;

    for (size_t i = g_mime_count; i > 0; i--) {
        if (strcasecmp(ext, g_mime[i - 1].ext) == 0)
            return g_mime[i - 1].type;
    }
    for (size_t i = 0; i < BUILTIN_MIME_COUNT; i++) {
        if (strcasecmp(ext, BUILTIN_MIME[i].ext) == 0)
            return BUILTIN_MIME[i].type;
    }
    return "application/octet-stream";
}

/* --- Initialization ----------------------------------------------------- */

static int parse_index_files(const char *list)
{
    g_index_count = 0;
    if (!list)
        return 0;
    const char *p = list;
    while (*p && g_index_count < INDEX_MAX) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
            len--;
        if (len > 0 && len < INDEX_NAME_MAX &&
            memchr(start, '/', len) == NULL && start[0] != '.') {
            memcpy(g_index[g_index_count], start, len);
            g_index[g_index_count][len] = '\0';
            g_index_count++;
        }
    }
    return 0;
}

int path_resolver_init(const char *root, const char *index_files,
                       const char *mime_file, int hidden_allowed,
                       int symlinks_allowed)
{
    if (g_root_fd >= 0) {
        close(g_root_fd);
        g_root_fd = -1;
    }
    free(g_mime);
    g_mime = NULL;
    g_mime_count = 0;
    g_mime_cap = 0;

    g_hidden_allowed = hidden_allowed ? 1 : 0;
    g_symlinks_allowed = symlinks_allowed ? 1 : 0;
    parse_index_files(index_files);

    int fd = open(root ? root : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "FATAL: document root '%s' is not an accessible "
                        "directory: %s\n", root ? root : ".", strerror(errno));
        return -1;
    }
    g_root_fd = fd;

    if (mime_file && *mime_file) {
        if (load_mime_file(mime_file) != 0) {
            close(g_root_fd);
            g_root_fd = -1;
            return -1;
        }
    }
    return 0;
}

void path_resolver_shutdown(void)
{
    if (g_root_fd >= 0) {
        close(g_root_fd);
        g_root_fd = -1;
    }
    free(g_mime);
    g_mime = NULL;
    g_mime_count = 0;
    g_mime_cap = 0;
}

/* --- Decoding and normalization ----------------------------------------- */

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode exactly once, rejecting NUL, backslash, DEL and controls. */
static PathStatus decode_path(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = in; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            int hi = hex_val((unsigned char)p[1]);
            int lo = hex_val((unsigned char)p[2]);
            if (hi < 0 || lo < 0)
                return PATH_ERR_MALFORMED;
            c = (unsigned char)((hi << 4) | lo);
            p += 3;
        } else {
            p++;
        }
        if (c == '\0' || c == '\\' || c < 0x20 || c == 0x7f)
            return PATH_ERR_MALFORMED;
        if (n + 1 >= cap)
            return PATH_ERR_TOO_LONG;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return PATH_OK;
}

/*
 * Normalize an absolute, decoded path into a root-relative form. `.` segments
 * are dropped, `..` pops the previous segment, and a `..` with nothing to pop
 * (an attempt to climb above the root) is refused. Hidden segments are
 * refused unless allowed. The empty result denotes the root directory.
 */
static PathStatus normalize_path(const char *decoded, char *rel, size_t cap)
{
    if (decoded[0] != '/')
        return PATH_ERR_MALFORMED;

    size_t out = 0;
    const char *p = decoded;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t seglen = (size_t)(p - seg);

        if (seglen == 1 && seg[0] == '.')
            continue;
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (out == 0)
                return PATH_ERR_FORBIDDEN;
            while (out > 0 && rel[out - 1] != '/')
                out--;
            if (out > 0)
                out--; /* drop the separating slash */
            continue;
        }
        if (seg[0] == '.' && !g_hidden_allowed)
            return PATH_ERR_FORBIDDEN;

        if (out > 0) {
            if (out + 1 >= cap)
                return PATH_ERR_TOO_LONG;
            rel[out++] = '/';
        }
        if (out + seglen >= cap)
            return PATH_ERR_TOO_LONG;
        memcpy(rel + out, seg, seglen);
        out += seglen;
    }
    rel[out] = '\0';
    return PATH_OK;
}

/* --- Descriptor opening ------------------------------------------------- */

/* Open `rel` beneath the root, returning a directory fd for the root itself. */
static int open_relative(const char *rel)
{
    /* The root directory itself decodes to the empty relative path; openat2
     * rejects an empty pathname, so reopen the root by descriptor. */
    if (*rel == '\0')
        return dup(g_root_fd);

#if defined(__linux__)
    unsigned long long flags = O_RDONLY | O_CLOEXEC;
    unsigned long long resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    if (!g_symlinks_allowed)
        resolve |= RESOLVE_NO_SYMLINKS;
    int fd = sys_openat2(g_root_fd, rel, flags, resolve);
    if (fd >= 0 || errno != ENOSYS)
        return fd;
#endif

    if (g_symlinks_allowed) {
        /*
         * Without openat2 we cannot both follow symlinks and prove they stay
         * beneath the root, so the fallback refuses symlinks entirely. This is
         * a documented limitation of pre-5.6 kernels.
         */
    }

    int dirfd = dup(g_root_fd);
    if (dirfd < 0)
        return -1;
    if (*rel == '\0')
        return dirfd;

    const char *p = rel;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        int last = (slash == NULL);
        char seg[INDEX_NAME_MAX];
        if (seglen == 0 || seglen >= sizeof(seg)) {
            close(dirfd);
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';
        int next = openat(dirfd, seg,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                              (last ? 0 : O_DIRECTORY));
        if (next < 0) {
            int err = errno;
            close(dirfd);
            errno = err;
            return -1;
        }
        close(dirfd);
        dirfd = next;
        if (last)
            break;
        p = slash + 1;
    }
    return dirfd;
}

static void fill_from_stat(ResolvedFile *out, const struct stat *sb,
                           const char *rel_path)
{
    out->fd     = -1;
    out->size   = sb->st_size;
    out->mtime  = sb->st_mtime;
    out->inode  = sb->st_ino;
    out->device = sb->st_dev;
    snprintf(out->path, sizeof(out->path), "%s", rel_path);
}

/*
 * Build "<rel>/<name>" (or just "<name>" when rel is empty) into `dst`,
 * truncating to fit. `rel` is already bounded to nearly the whole buffer, so
 * the recorded path is best-effort metadata; the opened file is identified by
 * its inode/device, not this string. Built with bounded copies rather than
 * snprintf so _FORTIFY_SOURCE's format-truncation analysis is satisfied.
 */
static void join_index_path(char *dst, size_t cap, const char *rel,
                            const char *name)
{
    size_t n = 0;
    if (rel && *rel) {
        size_t rl = strlen(rel);
        if (rl > cap - 1)
            rl = cap - 1;
        memcpy(dst, rel, rl);
        n = rl;
        if (n < cap - 1)
            dst[n++] = '/';
    }
    size_t nl = strlen(name);
    if (nl > cap - 1 - n)
        nl = cap - 1 - n;
    memcpy(dst + n, name, nl);
    n += nl;
    dst[n] = '\0';
}

/* Resolve a directory to an index file, filling `out` on success. */
static PathStatus open_index(int dirfd, const char *rel, ResolvedFile *out)
{
    for (size_t i = 0; i < g_index_count; i++) {
        int fd = openat(dirfd, g_index[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            continue;
        struct stat sb;
        if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode)) {
            char combined[PATH_MAX_BYTES];
            join_index_path(combined, sizeof(combined), rel, g_index[i]);
            fill_from_stat(out, &sb, combined);
            out->fd = fd;
            return PATH_OK;
        }
        close(fd);
    }
    return PATH_ERR_NOT_FOUND;
}

static PathStatus errno_to_status(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return PATH_ERR_NOT_FOUND;
    case ELOOP:
    case EACCES:
    case EPERM:
        return PATH_ERR_FORBIDDEN;
    case ENAMETOOLONG:
        return PATH_ERR_TOO_LONG;
    default:
        return PATH_ERR_NOT_FOUND;
    }
}

PathStatus path_resolver_open(const char *path, ResolvedFile *out)
{
    out->fd = -1;
    out->size = 0;
    out->mtime = 0;
    out->inode = 0;
    out->device = 0;
    out->path[0] = '\0';

    if (g_root_fd < 0)
        return PATH_ERR_UNAVAILABLE;

    char decoded[PATH_MAX_BYTES];
    PathStatus st = decode_path(path, decoded, sizeof(decoded));
    if (st != PATH_OK)
        return st;

    char rel[PATH_MAX_BYTES];
    st = normalize_path(decoded, rel, sizeof(rel));
    if (st != PATH_OK)
        return st;

    int fd = open_relative(rel);
    if (fd < 0)
        return errno_to_status(errno);

    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return PATH_ERR_NOT_FOUND;
    }
    if (S_ISDIR(sb.st_mode)) {
        PathStatus idx = open_index(fd, rel, out);
        close(fd);
        return idx;
    }
    if (!S_ISREG(sb.st_mode)) {
        close(fd);
        return PATH_ERR_FORBIDDEN;
    }
    fill_from_stat(out, &sb, rel);
    out->fd = fd;
    return PATH_OK;
}

void path_resolver_close(ResolvedFile *file)
{
    if (file && file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
}
