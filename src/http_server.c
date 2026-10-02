#define _GNU_SOURCE

#include "http_server.h"
#include "file_cache.h"
#include "log.h"
#include "metrics.h"
#include "path_resolver.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

/* --- Parser limits --- */
#define MAX_HEADERS 64
#define MAX_HEADER_LEN 1024
/* Leading empty lines tolerated before a request-line (RFC 9112 §2.2). */
#define MAX_LEADING_EMPTY_LINES 10

#define SERVER_TOKEN "SimpleHTTPServer/1.0"
#define DEFAULT_CONTENT_TYPE "text/html"

/* The legacy /home and /hello aliases are backed by these startup-cached
 * assets. They live under the shipped document root (see DOCUMENT_ROOT and
 * http_server.conf) rather than the working directory. */
#define HOME_ASSET_PATH  "root/home.html"
#define HELLO_ASSET_PATH "root/hello.html"

/* Bounded C-string copy that cannot trigger format-truncation warnings. */
static void copy_cstr(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
}
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* --- Small string helpers ------------------------------------------------ */

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
}
    return s;
}

static void rtrim(char *s, const char *set)
{
    char *end = s + strlen(s);
    while (end > s && strchr(set, end[-1])) {
        *--end = '\0';
}
}

static int token_char(unsigned char c)
{
    if (isalnum(c)) {
        return 1;
}
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int valid_token(const char *s)
{
    if (!s || !*s) {
        return 0;
}
    for (; *s; s++) {
        if (!token_char((unsigned char)*s)) {
            return 0;
}
    }
    return 1;
}

/*
 * Reject bytes that must never appear inside a request line or header field:
 * NUL, any control character except HTAB, and DEL.  `n` is the true line
 * length reported by the ring-buffer line reader, which is reliable even when
 * the line did not fit the destination buffer.
 */
static int contains_bad_ctl(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0') {
            return 1;
}
        if (c < 0x20 && c != '\t') {
            return 1;
}
        if (c == 0x7f) {
            return 1;
}
    }
    return 0;
}

static int parse_ull(const char *s, unsigned long long *out)
{
    if (!s || !*s) {
        return -1;
}
    unsigned long long v = 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return -1;
}
        unsigned d = (unsigned)(*s - '0');
        if (v > (ULLONG_MAX - d) / 10ULL) {
            return -1;
}
        v = v * 10ULL + d;
    }
    *out = v;
    return 0;
}

/* --- HTTP date helpers (RFC 9110 IMF-fixdate, locale-independent) -------- */

static const char *const WEEKDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void format_http_date(time_t t, char *buf, size_t cap)
{
    struct tm tm;
    if (!gmtime_r(&t, &tm)) {
        copy_cstr(buf, cap, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }
    int wday = tm.tm_wday % 7;
    if (wday < 0) {
        wday += 7;
}
    int mon = tm.tm_mon % 12;
    if (mon < 0) {
        mon += 12;
}
    snprintf(buf, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             WEEKDAYS[wday], tm.tm_mday, MONTHS[mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int month_index(const char *abbr)
{
    for (int i = 0; i < 12; i++) {
        if (strcasecmp(abbr, MONTHS[i]) == 0) {
            return i;
}
    }
    return -1;
}

static int parse_fixed_uint(const char **pp, int ndigits, int *out)
{
    const char *p = *pp;
    int v = 0;
    for (int i = 0; i < ndigits; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return -1;
}
        v = v * 10 + (p[i] - '0');
    }
    *pp = p + ndigits;
    *out = v;
    return 0;
}

/* Parse the fixed-width IMF-fixdate form "Sun, 06 Nov 1994 08:49:37 GMT". The
 * obsolete RFC 850 and asctime forms are ignored (the request is treated as
 * unconditional), which is permitted for a recipient. */
static int parse_http_date(const char *s, time_t *out)
{
    if (!s || strlen(s) < 29) {
        return -1;
}
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)p[i])) {
            return -1;
}
    }
    p += 3;
    if (p[0] != ',' || p[1] != ' ') {
        return -1;
}
    p += 2;
    int day;
    if (parse_fixed_uint(&p, 2, &day) != 0 || *p != ' ') {
        return -1;
}
    p++;
    char mon[4] = {p[0], p[1], p[2], '\0'};
    for (int i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)mon[i])) {
            return -1;
}
    }
    int mi = month_index(mon);
    if (mi < 0) {
        return -1;
}
    p += 3;
    if (*p != ' ') {
        return -1;
}
    p++;
    int year, hour, min, sec;
    if (parse_fixed_uint(&p, 4, &year) != 0 || *p != ' ') {
        return -1;
}
    p++;
    if (parse_fixed_uint(&p, 2, &hour) != 0 || *p != ':') {
        return -1;
}
    p++;
    if (parse_fixed_uint(&p, 2, &min) != 0 || *p != ':') {
        return -1;
}
    p++;
    if (parse_fixed_uint(&p, 2, &sec) != 0 || strcmp(p, " GMT") != 0) {
        return -1;
}
    if (day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60) {
        return -1;
}
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon  = mi;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) {
        return -1;
}
    *out = t;
    return 0;
}

/* Current time as an IMF-fixdate string. Cached per thread for one second so
 * the hot path does not call gmtime on every request. */
static const char *http_date_now(void)
{
    static _Thread_local char buf[40];
    static _Thread_local time_t cached_at = 0;
    time_t now = time(NULL);
    if (now != cached_at) {
        format_http_date(now, buf, sizeof(buf));
        cached_at = now;
    }
    return buf;
}

/* --- Request model (private to the parser) ------------------------------- */

typedef struct {
    char method[16];
    char target[1024];        /* raw request-target */
    char path[1024];          /* routing path (query/fragment stripped) */
    int  version_minor;       /* 0 or 1, or -1 when unrecognized */
    int  http10;
    int  keep_alive;
    int  is_head;
    int  asterisk;            /* request-target == "*" */
    int  absolute_form;

    int  has_host;
    int  has_transfer_encoding;
    int  has_content_length;
    long long content_length;
    int  expect_continue;
    int  expect_unsupported;

    int  has_accept_encoding;
    int  has_if_none_match;
    int  has_if_modified_since;
    int  has_if_range;
    int  has_range;
    char accept_encoding[256];
    char if_none_match[256];
    char if_modified_since[64];
    char if_range[256];
    char range[256];
} HTTPRequest;

/* --- Cached static asset (identity and gzip variants) -------------------- */

typedef struct {
    Representation plain;
    Representation gzip;
} StaticAsset;

static StaticAsset home_asset;
static StaticAsset hello_asset;
static int static_responses_initialized;

/* Phase 2: bounded cache for document-root representations. */
static FileCache *g_cache;

/*
 * Phase 5: per-request correlation ids. A relaxed atomic counter seeded once at
 * startup from the wall clock and pid makes ids unique within a run and
 * disjoint across near-simultaneous restarts without any locking on the hot
 * path. The value is rendered as 16 hex digits.
 */
static _Atomic unsigned long long g_request_seq;

static void seed_request_ids(void)
{
    unsigned long long seed =
        ((unsigned long long)time(NULL) << 20) ^ (unsigned long long)getpid();
    atomic_store_explicit(&g_request_seq, seed, memory_order_relaxed);
}

static void next_request_id(char *out, size_t cap)
{
    unsigned long long n = atomic_fetch_add_explicit(
        &g_request_seq, 1, memory_order_relaxed);
    snprintf(out, cap, "%016llx", n);
}

/* Build the X-Request-Id header line; empty when the request has no id. */
static void request_id_header(const PendingResponse *pr, char *out, size_t cap)
{
    if (cap == 0)
        return;
    if (pr->request_id[0])
        snprintf(out, cap, "X-Request-Id: %s\r\n", pr->request_id);
    else
        out[0] = '\0';
}

/* Phase 5: observability endpoints, frozen from the config at startup. */
static int  g_obs_enabled;
static char g_metrics_path[OBS_PATH_MAX];
static char g_health_path[OBS_PATH_MAX];
static char g_readiness_path[OBS_PATH_MAX];

static int path_is(const char *path, const char *configured)
{
    return configured[0] != '\0' && strcmp(path, configured) == 0;
}

static void repr_free(Representation *rep)
{
    free(rep->body);
    memset(rep, 0, sizeof(*rep));
}

static int read_asset(const char *path, unsigned char **body, size_t *body_len,
                      time_t *mtime)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
}

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t length = (size_t)st.st_size;
    unsigned char *data = malloc(length ? length : 1);
    if (!data) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, data + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
}
        if (count <= 0) {
            free(data);
            close(fd);
            errno = EIO;
            return -1;
        }
        offset += (size_t)count;
    }
    close(fd);
    *body = data;
    *body_len = length;
    *mtime = st.st_mtime;
    return 0;
}

static int gzip_asset(const unsigned char *input, size_t input_len,
                      unsigned char **output, size_t *output_len)
{
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        errno = EIO;
        return -1;
    }

    /* deflateBound(), not compressBound(): the latter sizes zlib framing and
     * under-estimates the gzip wrapper, which overflows for very small inputs. */
    uLong bound = deflateBound(&stream, (uLong)input_len);
    unsigned char *data = malloc(bound ? (size_t)bound : 1);
    if (!data) {
        deflateEnd(&stream);
        errno = ENOMEM;
        return -1;
    }

    stream.next_in = (Bytef *)input;
    stream.avail_in = (uInt)input_len;
    stream.next_out = data;
    stream.avail_out = (uInt)bound;
    int result = deflate(&stream, Z_FINISH);
    size_t produced = (size_t)stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        free(data);
        errno = EIO;
        return -1;
    }
    *output = data;
    *output_len = produced;
    return 0;
}

/* Build the strong validator and Last-Modified string for one representation.
 * The GZIP variant gets a distinct ETag because ETag identifies the selected
 * representation, not the underlying resource. The inode is folded in so a
 * same-second, same-size replacement still changes the validator. */
static void repr_set_validators_inode(Representation *rep, time_t mtime,
                                      ino_t inode, int gzip)
{
    rep->mtime = mtime;
    rep->inode = inode;
    format_http_date(mtime, rep->last_modified, sizeof(rep->last_modified));
    snprintf(rep->etag, sizeof(rep->etag), "\"%llx-%llx-%zx%s\"",
             (unsigned long long)mtime, (unsigned long long)inode, rep->body_len,
             gzip ? "-gzip" : "");
}

static void repr_set_validators(Representation *rep, time_t mtime, int gzip)
{
    repr_set_validators_inode(rep, mtime, 0, gzip);
}

static int load_static_asset(const char *path, StaticAsset *asset)
{
    memset(asset, 0, sizeof(*asset));
    time_t mtime = 0;
    if (read_asset(path, &asset->plain.body, &asset->plain.body_len, &mtime) != 0 ||
        gzip_asset(asset->plain.body, asset->plain.body_len,
                   &asset->gzip.body, &asset->gzip.body_len) != 0) {
        repr_free(&asset->plain);
        repr_free(&asset->gzip);
        return -1;
    }
    repr_set_validators(&asset->plain, mtime, 0);
    repr_set_validators(&asset->gzip, mtime, 1);
    return 0;
}

int initialize_static_responses(const ServerConfig *cfg)
{
    if (static_responses_initialized) {
        return 0;
    }
    if (load_static_asset(HOME_ASSET_PATH, &home_asset) != 0) {
        fprintf(stderr, "Failed to cache %s: %s\n", HOME_ASSET_PATH,
                strerror(errno));
        return -1;
    }
    if (load_static_asset(HELLO_ASSET_PATH, &hello_asset) != 0) {
        fprintf(stderr, "Failed to cache %s: %s\n", HELLO_ASSET_PATH,
                strerror(errno));
        repr_free(&home_asset.plain);
        repr_free(&home_asset.gzip);
        return -1;
    }

    if (path_resolver_init(cfg->document_root, cfg->index_files,
                           cfg->mime_types_file, cfg->hidden_files_allowed,
                           cfg->symlinks_allowed) != 0) {
        repr_free(&home_asset.plain);
        repr_free(&home_asset.gzip);
        repr_free(&hello_asset.plain);
        repr_free(&hello_asset.gzip);
        return -1;
    }

    seed_request_ids();
    g_obs_enabled = cfg->observability_enabled;
    snprintf(g_metrics_path, sizeof(g_metrics_path), "%s", cfg->metrics_path);
    snprintf(g_health_path, sizeof(g_health_path), "%s", cfg->health_path);
    snprintf(g_readiness_path, sizeof(g_readiness_path), "%s",
             cfg->readiness_path);

    size_t budget = (size_t)cfg->cache_budget_bytes;
    g_cache = file_cache_create(budget, CACHE_MAX_ENTRIES);
    if (!g_cache && budget > 0) {
        fprintf(stderr, "Failed to allocate the representation cache\n");
        path_resolver_shutdown();
        repr_free(&home_asset.plain);
        repr_free(&home_asset.gzip);
        repr_free(&hello_asset.plain);
        repr_free(&hello_asset.gzip);
        return -1;
    }

    static_responses_initialized = 1;
    return 0;
}

void shutdown_static_responses(void)
{
    if (!static_responses_initialized)
        return;
    file_cache_destroy(g_cache);
    g_cache = NULL;
    path_resolver_shutdown();
    repr_free(&home_asset.plain);
    repr_free(&home_asset.gzip);
    repr_free(&hello_asset.plain);
    repr_free(&hello_asset.gzip);
    static_responses_initialized = 0;
}

void http_server_cache_stats(long *bytes, long *entries)
{
    if (bytes)
        *bytes = g_cache ? (long)file_cache_bytes(g_cache) : 0;
    if (entries)
        *entries = g_cache ? (long)file_cache_entry_count(g_cache) : 0;
}

/*
 * Bind and listen on `port`. Shared by the plaintext and TLS listeners so
 * their socket options stay identical.
 */
static int create_listener(int port, int backlog, int reuseport)
{
    /* Ignore SIGPIPE globally: writing to a closed client must not kill the
     * server. */
    signal(SIGPIPE, SIG_IGN);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return -1;
    }

    int fl = fcntl(server_socket, F_GETFD);
    if (fl >= 0) {
        fcntl(server_socket, F_SETFD, fl | FD_CLOEXEC);
}

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt reuseaddr");
        close(server_socket);
        return -1;
    }

#if defined(SO_REUSEPORT)
    if (reuseport &&
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt reuseport");
        close(server_socket);
        return -1;
    }
#else
    (void)reuseport;
#endif

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return -1;
    }

    if (listen(server_socket, backlog) < 0) {
        perror("listen");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

int create_server_socket(const ServerConfig *cfg, int reuseport)
{
    return create_listener(cfg->port, cfg->backlog, reuseport);
}

int create_tls_server_socket(const ServerConfig *cfg, int reuseport)
{
    return create_listener(cfg->tls_port, cfg->backlog, reuseport);
}

/* --- Response construction ----------------------------------------------- */

static const char *reason_phrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

static void html_error_body(char *buf, size_t cap, int status, const char *detail)
{
    snprintf(buf, cap,
             "<html><head><title>%d %s</title></head>"
             "<body><h1>%d %s</h1><p>%s</p></body></html>\n",
             status, reason_phrase(status), status, reason_phrase(status), detail);
}

/*
 * Format a complete response with an optional text body embedded after the
 * headers into pr->header_buf. A NULL/empty body means a bodyless status
 * (204/304) and omits both Content-Type and Content-Length.
 */
static void format_simple(PendingResponse *pr, int status, int keep_alive,
                          int force_close, const char *extra,
                          const char *body, size_t body_len)
{
    int ka = keep_alive && !force_close;
    const char *conn = ka ? "keep-alive" : "close";
    char rid[40];
    request_id_header(pr, rid, sizeof(rid));
    int n;

    if (body && body_len) {
        n = snprintf(pr->header_buf, sizeof(pr->header_buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_TOKEN "\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "%s"
                     "Content-Type: " DEFAULT_CONTENT_TYPE "\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     status, reason_phrase(status), http_date_now(), conn,
                     rid, extra ? extra : "", body_len, body);
    } else {
        n = snprintf(pr->header_buf, sizeof(pr->header_buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_TOKEN "\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "%s"
                     "\r\n",
                     status, reason_phrase(status), http_date_now(), conn,
                     rid, extra ? extra : "");
    }

    if (n < 0 || (size_t)n >= sizeof(pr->header_buf)) {
        static const char fallback[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
            "Server: " SERVER_TOKEN "\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n\r\n";
        memcpy(pr->header_buf, fallback, sizeof(fallback) - 1);
        pr->header_len = sizeof(fallback) - 1;
        pr->header = NULL;
        pr->body = NULL;
        pr->body_len = 0;
        pr->body_fd = -1;
        pr->cache_entry = NULL;
        pr->is_head = 0;
        pr->force_close = 1;
        pr->status = 500;
        return;
    }

    pr->header = NULL;
    pr->header_len = (size_t)n;
    pr->body = NULL;
    pr->body_len = 0;
    pr->body_fd = -1;
    pr->cache_entry = NULL;
    pr->is_head = 0;
    pr->force_close = force_close;
    pr->status = status;
}

static void emit_status(PendingResponse *pr, int status, int keep_alive,
                        int force_close, const char *extra, const char *detail)
{
    char body[256];
    html_error_body(body, sizeof(body), status, detail);
    format_simple(pr, status, keep_alive, force_close, extra, body, strlen(body));
}

/*
 * Format a representation response (200 or 206) whose body starts `start`
 * bytes into `rep`. `body` is the memory slice for a memory-backed
 * representation, or NULL when `rep` is file-backed: in that case the
 * descriptor is transferred to the response and the event loop streams it with
 * sendfile(). `is_partial` adds a Content-Range header. body_len always carries
 * the Content-Length a client should expect, even for HEAD.
 */
static void format_asset(PendingResponse *pr, int status, int keep_alive,
                         int force_close, const Representation *rep,
                         const char *content_type,
                         const unsigned char *body, size_t start,
                         size_t body_len, int is_partial, int is_head, int gzip)
{
    int ka = keep_alive && !force_close;
    const char *conn = ka ? "keep-alive" : "close";
    char range_hdr[80];
    range_hdr[0] = '\0';
    if (is_partial && body_len > 0) {
        snprintf(range_hdr, sizeof(range_hdr),
                 "Content-Range: bytes %zu-%zu/%zu\r\n",
                 start, start + body_len - 1, rep->body_len);
    }

    char rid[40];
    request_id_header(pr, rid, sizeof(rid));
    int n = snprintf(pr->header_buf, sizeof(pr->header_buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_TOKEN "\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "Content-Type: %s\r\n"
                     "%s"
                     "Content-Length: %zu\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "%s"
                     "ETag: %s\r\n"
                     "Last-Modified: %s\r\n"
                     "Vary: Accept-Encoding\r\n"
                     "\r\n",
                     status, reason_phrase(status), http_date_now(), conn,
                     rid, content_type,
                     gzip ? "Content-Encoding: gzip\r\n" : "",
                     body_len, range_hdr, rep->etag, rep->last_modified);

    if (n < 0 || (size_t)n >= sizeof(pr->header_buf)) {
        emit_status(pr, 500, keep_alive, 1, NULL, "Response header overflow");
        return;
    }

    pr->header = NULL;
    pr->header_len = (size_t)n;
    pr->body = body;
    pr->body_len = body_len;
    pr->body_fd = -1;
    if (!body && rep->fd >= 0) {
        pr->body_fd = rep->fd;
        pr->body_file_off = rep->file_off + (off_t)start;
    }
    pr->is_head = is_head;
    pr->force_close = force_close;
    pr->status = status;
}

/* --- Request-line and header parsing ------------------------------------- */

static int parse_request_line(char *line, size_t len, HTTPRequest *req)
{
    if (contains_bad_ctl(line, len)) {
        return 400;
}

    char *p = line;
    char *m_end = p;
    while (*m_end && *m_end != ' ' && *m_end != '\t') {
        m_end++;
}
    if (m_end == p) {
        return 400;
}
    if ((size_t)(m_end - p) >= sizeof(req->method)) {
        return 400;
}
    memcpy(req->method, p, (size_t)(m_end - p));
    req->method[m_end - p] = '\0';
    if (!valid_token(req->method)) {
        return 400;
}

    p = m_end;
    while (*p == ' ' || *p == '\t') {
        p++;
}
    if (*p == '\0') {
        return 400;
}

    char *t_end = p;
    while (*t_end && *t_end != ' ' && *t_end != '\t') {
        t_end++;
}
    if (t_end == p) {
        return 400;
}
    if ((size_t)(t_end - p) >= sizeof(req->target)) {
        return 414;
}
    memcpy(req->target, p, (size_t)(t_end - p));
    req->target[t_end - p] = '\0';

    p = t_end;
    while (*p == ' ' || *p == '\t') {
        p++;
}
    if (*p == '\0') {
        return 400; /* HTTP/0.9 and missing versions are not served */
}

    char *v_end = p;
    while (*v_end && *v_end != ' ' && *v_end != '\t') {
        v_end++;
}
    char saved = *v_end;
    *v_end = '\0';
    int status = 0;
    if (strcasecmp(p, "HTTP/1.1") == 0) {
        req->version_minor = 1;
        req->keep_alive = 1;
    } else if (strcasecmp(p, "HTTP/1.0") == 0) {
        req->version_minor = 0;
        req->http10 = 1;
        req->keep_alive = 0;
    } else {
        status = 505;
    }
    *v_end = saved;
    if (status != 0) {
        return status;
}

    char *q = v_end;
    while (*q == ' ' || *q == '\t') {
        q++;
}
    if (*q != '\0') {
        return 400;
}

    return 0;
}

static int parse_target(HTTPRequest *req)
{
    const char *t = req->target;
    const char *path = NULL;

    if (t[0] == '*') {
        if (t[1] != '\0') {
            return 400;
}
        req->asterisk = 1;
        copy_cstr(req->path, sizeof(req->path), "*");
        return 0;
    }

    if (t[0] == '/') {
        path = t;
    } else {
        const char *scheme = strstr(t, "://");
        if (!scheme) {
            return 400; /* authority-form (CONNECT) is not supported */
}
        const char *auth = scheme + 3;
        if (*auth == '\0') {
            return 400;
}
        req->absolute_form = 1;
        const char *slash = strchr(auth, '/');
        path = slash ? slash : "/";
    }

    size_t n = 0;
    for (const char *s = path; *s && *s != '?' && *s != '#'; s++) {
        if (n + 1 >= sizeof(req->path)) {
            return 414;
}
        req->path[n++] = *s;
    }
    req->path[n] = '\0';
    if (n == 0) {
        copy_cstr(req->path, sizeof(req->path), "/");
}
    return 0;
}

static void parse_accept_encoding(const char *value, int *gzip_ok, int *identity_ok)
{
    *gzip_ok = 0;
    *identity_ok = 1; /* identity is acceptable unless explicitly refused */
    if (!value) {
        return;
}

    char copy[256];
    copy_cstr(copy, sizeof(copy), value);

    int gzip_seen = 0, identity_seen = 0, star_seen = 0, star_q0 = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        tok = skip_ws(tok);
        rtrim(tok, " \t");

        double q = 1.0;
        char *psave = NULL;
        char *semi = strchr(tok, ';');
        if (semi) {
            *semi = '\0';
            rtrim(tok, " \t");
            for (char *param = strtok_r(semi + 1, ";", &psave); param;
                 param = strtok_r(NULL, ";", &psave)) {
                param = skip_ws(param);
                rtrim(param, " \t");
                char *eq = strchr(param, '=');
                if (!eq) {
                    continue;
}
                *eq = '\0';
                if (strcasecmp(param, "q") != 0) {
                    continue;
}
                char *v = skip_ws(eq + 1);
                rtrim(v, " \t");
                char *end = NULL;
                q = strtod(v, &end);
                if (end == v || (end && *end != '\0') || q < 0.0 || q > 1.0) {
                    q = 0.0;
}
            }
        }

        if (strcasecmp(tok, "gzip") == 0) {
            gzip_seen = 1;
            *gzip_ok = q > 0.0;
        } else if (strcasecmp(tok, "identity") == 0) {
            identity_seen = 1;
            *identity_ok = q > 0.0;
        } else if (strcmp(tok, "*") == 0) {
            star_seen = 1;
            star_q0 = q <= 0.0;
        }
    }

    if (star_seen) {
        if (!gzip_seen && !star_q0) {
            *gzip_ok = 1;
}
        if (!identity_seen && star_q0) {
            *identity_ok = 0;
}
    }
}

/*
 * Parse and validate one header line. Returns 0 on success or an HTTP error
 * status. Obs-fold (a leading SP/HTAB) is rejected by the caller.
 */
static int parse_header_line(char *line, size_t len, HTTPRequest *req)
{
    if (contains_bad_ctl(line, len)) {
        return 400;
}

    char *colon = strchr(line, ':');
    if (!colon || colon == line) {
        return 400;
}
    if (colon[-1] == ' ' || colon[-1] == '\t') {
        return 400; /* no whitespace between field-name and colon */
}
    *colon = '\0';
    if (!valid_token(line)) {
        return 400;
}

    char *name = line;
    char *value = skip_ws(colon + 1);
    rtrim(value, " \t");

    if (strcasecmp(name, "Content-Length") == 0) {
        unsigned long long cl;
        if (parse_ull(value, &cl) != 0 || cl > (unsigned long long)LLONG_MAX) {
            return 400;
}
        if (req->has_content_length) {
            return 400; /* duplicate Content-Length */
}
        req->has_content_length = 1;
        req->content_length = (long long)cl;
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        req->has_transfer_encoding = 1;
    } else if (strcasecmp(name, "Host") == 0) {
        if (req->has_host) {
            return 400; /* multiple Host headers */
}
        if (*value == '\0' || strpbrk(value, " \t") != NULL) {
            return 400;
}
        req->has_host = 1;
    } else if (strcasecmp(name, "Connection") == 0) {
        char copy[256];
        copy_cstr(copy, sizeof(copy), value);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            tok = skip_ws(tok);
            rtrim(tok, " \t");
            if (strcasecmp(tok, "close") == 0) {
                req->keep_alive = 0;
            } else if (strcasecmp(tok, "keep-alive") == 0) {
                req->keep_alive = 1;
}
        }
    } else if (strcasecmp(name, "Expect") == 0) {
        if (strcasecmp(value, "100-continue") == 0) {
            req->expect_continue = 1;
        } else {
            req->expect_unsupported = 1;
}
    } else if (strcasecmp(name, "Accept-Encoding") == 0) {
        copy_cstr(req->accept_encoding, sizeof(req->accept_encoding), value);
        req->has_accept_encoding = 1;
    } else if (strcasecmp(name, "If-None-Match") == 0) {
        copy_cstr(req->if_none_match, sizeof(req->if_none_match), value);
        req->has_if_none_match = 1;
    } else if (strcasecmp(name, "If-Modified-Since") == 0) {
        copy_cstr(req->if_modified_since, sizeof(req->if_modified_since), value);
        req->has_if_modified_since = 1;
    } else if (strcasecmp(name, "If-Range") == 0) {
        copy_cstr(req->if_range, sizeof(req->if_range), value);
        req->has_if_range = 1;
    } else if (strcasecmp(name, "Range") == 0) {
        copy_cstr(req->range, sizeof(req->range), value);
        req->has_range = 1;
    }
    return 0;
}

/* --- Conditional and range evaluation ------------------------------------ */

/* Weak entity-tag comparison over a comma-separated If-None-Match list. */
static int if_none_match_matches(const char *list, const char *etag)
{
    char copy[256];
    copy_cstr(copy, sizeof(copy), list);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        tok = skip_ws(tok);
        rtrim(tok, " \t");
        if (strcmp(tok, "*") == 0) {
            return 1;
}
        const char *t = tok;
        if (t[0] == 'W' && t[1] == '/') {
            t += 2;
}
        const char *e = etag;
        if (e[0] == 'W' && e[1] == '/') {
            e += 2;
}
        if (strcmp(t, e) == 0) {
            return 1;
}
    }
    return 0;
}

static int request_not_modified(const HTTPRequest *req, const Representation *rep)
{
    if (req->has_if_none_match) {
        return if_none_match_matches(req->if_none_match, rep->etag);
}
    if (req->has_if_modified_since) {
        time_t t;
        if (parse_http_date(req->if_modified_since, &t) == 0 && rep->mtime <= t) {
            return 1;
}
    }
    return 0;
}

static int if_range_matches(const HTTPRequest *req, const Representation *rep)
{
    const char *v = req->if_range;
    if (v[0] == '"' || (v[0] == 'W' && v[1] == '/')) {
        return strcmp(v, rep->etag) == 0; /* strong comparison */
}
    time_t t;
    if (parse_http_date(v, &t) != 0) {
        return 0;
}
    return rep->mtime == t;
}

typedef struct {
    size_t start;
    size_t end;   /* inclusive */
} ByteRange;

/*
 * Parse a `Range` header value against a representation of `len` bytes.
 *
 * On success (return 1) up to `cap` satisfiable ranges are written to
 * out[0..*count) in the order given; `*unsatisfiable` is set when at least one
 * syntactically valid range-spec addressed bytes outside the representation.
 *
 * Returns 0 when the header must be ignored: it is absent, not `bytes=`,
 * syntactically invalid, or contains more than `cap` satisfiable ranges. RFC
 * 9110 permits a server to ignore a Range header, and ignoring an oversized
 * one bounds per-request work.
 */
static int parse_byte_ranges(const char *value, size_t len, ByteRange *out,
                             int cap, int *count, int *unsatisfiable)
{
    *count = 0;
    *unsatisfiable = 0;
    if (!value || strncasecmp(value, "bytes=", 6) != 0) {
        return 0;
}
    const char *p = value + 6;
    int n = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *comma = strchr(p, ',');
        const char *spec_end = comma ? comma : p + strlen(p);
        const char *spec_last = spec_end;
        while (spec_last > p && (spec_last[-1] == ' ' || spec_last[-1] == '\t')) {
            spec_last--;
        }
        if (spec_last == p) {
            return 0; /* empty range-spec: invalid range-set */
}

        const char *dash = memchr(p, '-', (size_t)(spec_last - p));
        if (!dash) {
            return 0;
}
        size_t flen = (size_t)(dash - p);
        size_t llen = (size_t)(spec_last - (dash + 1));
        char first_buf[32];
        char last_buf[32];
        if (flen >= sizeof(first_buf) || llen >= sizeof(last_buf)) {
            return 0;
}
        memcpy(first_buf, p, flen);
        first_buf[flen] = '\0';
        memcpy(last_buf, dash + 1, llen);
        last_buf[llen] = '\0';

        int has_first = flen != 0;
        int has_last = llen != 0;
        if (!has_first && !has_last) {
            return 0; /* "-" is not a range-spec */
}
        unsigned long long first = 0;
        unsigned long long last = 0;
        if (has_first && parse_ull(first_buf, &first) != 0) {
            return 0;
}
        if (has_last && parse_ull(last_buf, &last) != 0) {
            return 0;
}

        size_t s = 0;
        size_t e = 0;
        if (!has_first) {
            if (last == 0 || len == 0) {
                *unsatisfiable = 1;
                goto next_spec;
            }
            s = (last >= len) ? 0 : len - (size_t)last;
            e = len - 1;
        } else {
            if (has_last && last < first) {
                return 0; /* last < first: invalid range-set */
            }
            if (len == 0 || first >= len) {
                *unsatisfiable = 1;
                goto next_spec;
            }
            s = (size_t)first;
            e = (!has_last || last >= len) ? len - 1 : (size_t)last;
        }

        if (n >= cap) {
            return 0; /* too many ranges: ignore the header */
        }
        out[n].start = s;
        out[n].end = e;
        n++;

next_spec:
        if (!comma) {
            break;
}
        p = comma + 1;
    }

    *count = n;
    return 1;
}

/* Boundary used to frame multipart/byteranges. RFC 9110 requires the value not
 * appear in the representation; format_multipart() checks the candidate against
 * the body and retries before falling back to the full response. */
#define MULTIPART_BOUNDARY_MAX 70

static void make_multipart_boundary(char *out, size_t cap)
{
    static _Thread_local unsigned long long counter = 0;
    snprintf(out, cap, "----SimpleHTTPServerBoundary%llx", ++counter);
}

static int bytes_contain(const unsigned char *hay, size_t hay_len,
                         const char *needle, size_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) {
        return 0;
}
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return 1;
}
    }
    return 0;
}

/* Copy `n` bytes at `off` from a representation into `dst`, whether the
 * representation is memory-backed or file-backed. Returns 0 on success. */
static int rep_copy(const Representation *rep, size_t off,
                    unsigned char *dst, size_t n)
{
    if (rep->body) {
        memcpy(dst, rep->body + off, n);
        return 0;
    }
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(rep->fd, dst + got, n - got,
                          rep->file_off + (off_t)(off + got));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Build a `multipart/byteranges` response body into a heap buffer the response
 * owns. Falls back to the full 200 body when the assembled size would exceed
 * MAX_MULTIPART_BYTES or allocation fails. Ranges are always identity. */
static void format_multipart(PendingResponse *pr, int keep_alive,
                             int force_close, const Representation *rep,
                             const char *content_type, const ByteRange *ranges,
                             int nranges, int is_head)
{
    /* Pick a boundary that does not occur in the representation, as RFC 9110
     * requires; give up after a few attempts and serve the full body. For a
     * file-backed representation we cannot scan cheaply, so the high-entropy
     * boundary is used as-is. */
    char boundary[MULTIPART_BOUNDARY_MAX];
    size_t blen = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        make_multipart_boundary(boundary, sizeof(boundary));
        blen = strlen(boundary);
        if (!rep->body || !bytes_contain(rep->body, rep->body_len, boundary, blen))
            break;
        blen = 0;
    }
    if (blen == 0) {
        format_asset(pr, 200, keep_alive, force_close, rep, content_type,
                     rep->body, 0, rep->body_len, 0, is_head, 0);
        return;
    }
    /* Worst case per part: boundary, content type, a 27-digit range header,
     * CRLFCRLF, and the part bytes plus CRLF. */
    size_t part_hdr = 2 + blen + 2 + strlen(content_type) + 64 + 64;

    size_t bound = 0;
    for (int i = 0; i < nranges; i++) {
        bound += part_hdr + (ranges[i].end - ranges[i].start) + 1 + 2;
    }
    bound += 2 + blen + 4; /* closing --boundary-- CRLF */

    if (bound > MAX_MULTIPART_BYTES || rep->body_len == 0) {
        format_asset(pr, 200, keep_alive, force_close, rep, content_type,
                     rep->body, 0, rep->body_len, 0, is_head, 0);
        return;
    }

    unsigned char *body = malloc(bound);
    if (!body) {
        format_asset(pr, 200, keep_alive, force_close, rep, content_type,
                     rep->body, 0, rep->body_len, 0, is_head, 0);
        return;
    }

    size_t off = 0;
    for (int i = 0; i < nranges; i++) {
        int n = snprintf((char *)body + off, bound - off,
                         "--%s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Range: bytes %zu-%zu/%zu\r\n"
                         "\r\n",
                         boundary, content_type, ranges[i].start, ranges[i].end,
                         rep->body_len);
        if (n < 0 || (size_t)n >= bound - off)
            goto fail;
        off += (size_t)n;
        size_t count = ranges[i].end - ranges[i].start + 1;
        if (rep_copy(rep, ranges[i].start, body + off, count) != 0)
            goto fail;
        off += count;
        body[off++] = '\r';
        body[off++] = '\n';
    }
    int tail = snprintf((char *)body + off, bound - off, "--%s--\r\n", boundary);
    if (tail < 0 || (size_t)tail >= bound - off)
        goto fail;
    off += (size_t)tail;

    int ka = keep_alive && !force_close;
    const char *conn = ka ? "keep-alive" : "close";
    char rid[40];
    request_id_header(pr, rid, sizeof(rid));
    int hn = snprintf(pr->header_buf, sizeof(pr->header_buf),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Date: %s\r\n"
                      "Server: " SERVER_TOKEN "\r\n"
                      "Connection: %s\r\n"
                      "%s"
                      "Content-Type: multipart/byteranges; boundary=%s\r\n"
                      "Content-Length: %zu\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "ETag: %s\r\n"
                      "Last-Modified: %s\r\n"
                      "Vary: Accept-Encoding\r\n"
                      "\r\n",
                      http_date_now(), conn, rid, boundary, off,
                      rep->etag, rep->last_modified);
    if (hn < 0 || (size_t)hn >= sizeof(pr->header_buf)) {
        free(body);
        emit_status(pr, 500, keep_alive, 1, NULL, "Response header overflow");
        return;
    }

    pr->header = NULL;
    pr->header_len = (size_t)hn;
    pr->body = body;
    pr->body_len = off;
    pr->body_fd = -1;
    pr->owned_body = body;
    pr->is_head = is_head;
    pr->force_close = force_close;
    pr->status = 206;
    return;

fail:
    free(body);
    format_asset(pr, 200, keep_alive, force_close, rep, content_type,
                 rep->body, 0, rep->body_len, 0, is_head, 0);
}

/* --- Method classification ----------------------------------------------- */

enum {
    METHOD_UNKNOWN = 0,
    METHOD_GET,
    METHOD_HEAD,
    METHOD_OPTIONS,
    METHOD_POST
};

static int method_class(const char *method)
{
    if (strcmp(method, "GET") == 0) {     return METHOD_GET;
}
    if (strcmp(method, "HEAD") == 0) {    return METHOD_HEAD;
}
    if (strcmp(method, "OPTIONS") == 0) { return METHOD_OPTIONS;
}
    if (strcmp(method, "POST") == 0) {    return METHOD_POST;
}
    return METHOD_UNKNOWN;
}

#define ALLOW_VALUE "Allow: GET, HEAD, OPTIONS\r\n"

/* --- Phase 5: observability endpoints ------------------------------------ */

/* Format a small text response. The body is borrowed (a literal or a heap
 * buffer the caller owns and assigns to pr->owned_body afterwards). */
static void format_observability(PendingResponse *pr, int status, int keep_alive,
                                 int force_close, const char *content_type,
                                 const char *body, size_t body_len, int is_head)
{
    int ka = keep_alive && !force_close;
    const char *conn = ka ? "keep-alive" : "close";
    char rid[40];
    request_id_header(pr, rid, sizeof(rid));
    int n = snprintf(pr->header_buf, sizeof(pr->header_buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: " SERVER_TOKEN "\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "\r\n",
                     status, reason_phrase(status), http_date_now(), conn,
                     rid, content_type, body_len);
    if (n < 0 || (size_t)n >= sizeof(pr->header_buf)) {
        emit_status(pr, 500, keep_alive, 1, NULL, "Response header overflow");
        return;
    }

    pr->header = NULL;
    pr->header_len = (size_t)n;
    pr->body = (const unsigned char *)body;
    pr->body_len = body_len;
    pr->body_fd = -1;
    pr->is_head = is_head;
    pr->force_close = force_close;
    pr->status = status;
}

static void serve_metrics(const HTTPRequest *req, int force_close,
                          PendingResponse *pr)
{
    char *body = malloc(METRICS_PROM_MAX);
    if (!body) {
        emit_status(pr, 500, req->keep_alive, 1, NULL,
                    "Metrics are unavailable");
        return;
    }
    size_t len = metrics_prometheus(body, METRICS_PROM_MAX);
    if (len >= METRICS_PROM_MAX)
        len = METRICS_PROM_MAX - 1;
    body[len] = '\0';
    log_msg(LOG_LEVEL_DEBUG, "metrics scrape bytes=%zu", len);

    format_observability(pr, 200, req->keep_alive, force_close,
                         "text/plain; version=0.0.4; charset=utf-8",
                         body, len, req->is_head);
    if (pr->status == 200 && pr->body == (const unsigned char *)body)
        pr->owned_body = (unsigned char *)body;
    else
        free(body);
}

static void serve_health(const HTTPRequest *req, int force_close,
                         PendingResponse *pr)
{
    static const char body[] = "ok\n";
    format_observability(pr, 200, req->keep_alive, force_close,
                         "text/plain; charset=utf-8", body, sizeof(body) - 1,
                         req->is_head);
}

static void serve_readiness(const HTTPRequest *req, int force_close,
                            PendingResponse *pr)
{
    static const char ready[] = "ready\n";
    static const char draining[] = "draining\n";
    if (metrics_is_ready()) {
        format_observability(pr, 200, req->keep_alive, force_close,
                             "text/plain; charset=utf-8", ready,
                             sizeof(ready) - 1, req->is_head);
    } else {
        format_observability(pr, 503, req->keep_alive, force_close,
                             "text/plain; charset=utf-8", draining,
                             sizeof(draining) - 1, req->is_head);
    }
}

/* --- Response selection -------------------------------------------------- */

static void select_asset_response(const HTTPRequest *req, StaticAsset *asset,
                                  const char *content_type, int force_close,
                                  PendingResponse *pr)
{
    int gzip_ok = 0, identity_ok = 1;
    parse_accept_encoding(req->has_accept_encoding ? req->accept_encoding : NULL,
                          &gzip_ok, &identity_ok);
    if (!gzip_ok && !identity_ok) {
        emit_status(pr, 406, req->keep_alive, 0, NULL,
                    "No acceptable content encoding is available");
        return;
    }

    int use_gzip = gzip_ok;
    const Representation *rep = use_gzip ? &asset->gzip : &asset->plain;

    if (request_not_modified(req, rep)) {
        char extra[160];
        snprintf(extra, sizeof(extra),
                 "ETag: %s\r\nLast-Modified: %s\r\nVary: Accept-Encoding\r\n",
                 rep->etag, rep->last_modified);
        format_simple(pr, 304, req->keep_alive, 0, extra, NULL, 0);
        return;
    }

    /*
     * Range requests are served from the identity representation so range
     * endpoints address decodable bytes and every multipart part is coherent.
     * When identity is not acceptable (identity;q=0) the Range header is
     * ignored and the negotiated representation is sent whole.
     */
    const Representation *range_rep = &asset->plain;
    int range_usable = req->has_range && identity_ok;

    ByteRange ranges[MAX_MULTIPART_RANGES];
    int nranges = 0, unsatisfiable = 0;
    int have_ranges = 0;
    if (range_usable &&
        (!req->has_if_range || if_range_matches(req, range_rep))) {
        have_ranges = parse_byte_ranges(req->range, range_rep->body_len, ranges,
                                        MAX_MULTIPART_RANGES, &nranges,
                                        &unsatisfiable);
    }

    if (have_ranges && nranges == 0 && unsatisfiable) {
        char extra[64];
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%zu\r\n",
                 range_rep->body_len);
        emit_status(pr, 416, req->keep_alive, 0, extra,
                    "The requested range cannot be satisfied");
        return;
    }

    if (have_ranges && nranges == 1) {
        size_t start = ranges[0].start;
        size_t end = ranges[0].end;
        size_t count = end - start + 1;
        format_asset(pr, 206, req->keep_alive, force_close, range_rep,
                     content_type, range_rep->body + start, start, count, 1,
                     req->is_head, 0);
        return;
    }

    if (have_ranges && nranges >= 2) {
        format_multipart(pr, req->keep_alive, force_close, range_rep,
                         content_type, ranges, nranges, req->is_head);
        return;
    }

    format_asset(pr, 200, req->keep_alive, force_close, rep, content_type,
                 rep->body, 0, rep->body_len, 0, req->is_head, use_gzip);
}

/* --- Phase 2: document-root serving -------------------------------------- */

/* Read exactly `size` bytes from `fd` starting at offset 0. */
static int read_fd_all(int fd, off_t size, unsigned char **body, size_t *body_len)
{
    size_t length = (size_t)size;
    unsigned char *data = malloc(length ? length : 1);
    if (!data) {
        errno = ENOMEM;
        return -1;
    }
    size_t off = 0;
    while (off < length) {
        ssize_t n = pread(fd, data + off, length - off, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(data);
            errno = EIO;
            return -1;
        }
        off += (size_t)n;
    }
    *body = data;
    *body_len = length;
    return 0;
}

/* Serve a file-backed representation (identity only) with the Phase 1
 * conditional and range semantics, streaming the body with sendfile(). On
 * return `rep->fd` is -1: the descriptor is either owned by the response or
 * was closed here. */
static void select_stream_response(const HTTPRequest *req, Representation *rep,
                                   const char *content_type, int force_close,
                                   PendingResponse *pr)
{
    int gzip_ok = 0, identity_ok = 1;
    parse_accept_encoding(req->has_accept_encoding ? req->accept_encoding : NULL,
                          &gzip_ok, &identity_ok);
    (void)gzip_ok;
    if (!identity_ok) {
        emit_status(pr, 406, req->keep_alive, 0, NULL,
                    "No acceptable content encoding is available");
        goto close_fd;
    }

    if (request_not_modified(req, rep)) {
        char extra[160];
        snprintf(extra, sizeof(extra),
                 "ETag: %s\r\nLast-Modified: %s\r\nVary: Accept-Encoding\r\n",
                 rep->etag, rep->last_modified);
        format_simple(pr, 304, req->keep_alive, 0, extra, NULL, 0);
        goto close_fd;
    }

    ByteRange ranges[MAX_MULTIPART_RANGES];
    int nranges = 0, unsatisfiable = 0, have_ranges = 0;
    if (req->has_range &&
        (!req->has_if_range || if_range_matches(req, rep))) {
        have_ranges = parse_byte_ranges(req->range, rep->body_len, ranges,
                                        MAX_MULTIPART_RANGES, &nranges,
                                        &unsatisfiable);
    }

    if (have_ranges && nranges == 0 && unsatisfiable) {
        char extra[64];
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%zu\r\n",
                 rep->body_len);
        emit_status(pr, 416, req->keep_alive, 0, extra,
                    "The requested range cannot be satisfied");
        goto close_fd;
    }

    if (have_ranges && nranges == 1) {
        size_t start = ranges[0].start;
        size_t count = ranges[0].end - ranges[0].start + 1;
        format_asset(pr, 206, req->keep_alive, force_close, rep, content_type,
                     NULL, start, count, 1, req->is_head, 0);
        rep->fd = -1; /* ownership moved into the response */
        return;
    }

    if (have_ranges && nranges >= 2) {
        format_multipart(pr, req->keep_alive, force_close, rep, content_type,
                         ranges, nranges, req->is_head);
        goto close_fd;
    }

    format_asset(pr, 200, req->keep_alive, force_close, rep, content_type,
                 NULL, 0, rep->body_len, 0, req->is_head, 0);
    rep->fd = -1;
    return;

close_fd:
    if (rep->fd >= 0) {
        close(rep->fd);
        rep->fd = -1;
    }
}

static void serve_resolved(const HTTPRequest *req, ResolvedFile *rf,
                           int force_close, PendingResponse *pr)
{
    const char *ctype = mime_type_for_path(rf->path);

    if (g_cache && rf->size >= 0 && (size_t)rf->size <= CACHE_MAX_FILE_BYTES) {
        struct CacheEntry *entry = file_cache_acquire(g_cache, rf->path);
        if (entry) {
            StaticAsset asset;
            asset.plain = *file_cache_plain(entry);
            asset.gzip = *file_cache_gzip(entry);
            path_resolver_close(rf);
            select_asset_response(req, &asset, ctype, force_close, pr);
            pr->cache_entry = entry;
            return;
        }

        /* Miss: read, precompress, and admit. */
        unsigned char *data = NULL;
        size_t len = 0;
        if (read_fd_all(rf->fd, rf->size, &data, &len) == 0) {
            Representation plain, gz;
            memset(&plain, 0, sizeof(plain));
            memset(&gz, 0, sizeof(gz));
            plain.body = data;
            plain.body_len = len;
            plain.fd = -1;
            if (gzip_asset(data, len, &gz.body, &gz.body_len) == 0) {
                gz.fd = -1;
                repr_set_validators_inode(&plain, rf->mtime, rf->inode, 0);
                repr_set_validators_inode(&gz, rf->mtime, rf->inode, 1);
                entry = file_cache_insert(g_cache, rf->path, &plain, &gz);
                if (entry) {
                    StaticAsset asset;
                    asset.plain = *file_cache_plain(entry);
                    asset.gzip = *file_cache_gzip(entry);
                    path_resolver_close(rf);
                    select_asset_response(req, &asset, ctype, force_close, pr);
                    pr->cache_entry = entry;
                    return;
                }
                free(plain.body);
                free(gz.body);
            } else {
                free(plain.body);
            }
        }
        /* Read/precompress/admit failed: fall back to streaming. */
    }

    Representation rep;
    memset(&rep, 0, sizeof(rep));
    rep.body_len = (size_t)rf->size;
    rep.fd = rf->fd;
    rep.file_off = 0;
    rep.mtime = rf->mtime;
    rep.inode = rf->inode;
    repr_set_validators_inode(&rep, rf->mtime, rf->inode, 0);
    rf->fd = -1; /* ownership moved into rep */
    select_stream_response(req, &rep, ctype, force_close, pr);
}

static void serve_document_root(const HTTPRequest *req, int force_close,
                                PendingResponse *pr)
{
    ResolvedFile rf;
    PathStatus st = path_resolver_open(req->path, &rf);
    switch (st) {
    case PATH_OK:
        serve_resolved(req, &rf, force_close, pr);
        return;
    case PATH_ERR_MALFORMED:
        emit_status(pr, 400, 0, 1, NULL, "Malformed request path");
        return;
    case PATH_ERR_TOO_LONG:
        emit_status(pr, 414, 0, 1, NULL, "Request path too long");
        return;
    case PATH_ERR_FORBIDDEN:
        emit_status(pr, 403, req->keep_alive, force_close, NULL,
                    "Access to the requested resource is forbidden");
        return;
    case PATH_ERR_NOT_FOUND:
    case PATH_ERR_UNAVAILABLE:
    default:
        /* Preserve the legacy root alias when the document root has no index. */
        if (strcmp(req->path, "/") == 0) {
            select_asset_response(req, &home_asset, DEFAULT_CONTENT_TYPE,
                                  force_close, pr);
            return;
        }
        emit_status(pr, 404, req->keep_alive, force_close, NULL,
                    "The requested resource was not found");
        return;
    }
}

/*
 * Parse one HTTP request from `rb` and populate `pr` with the response the
 * event loop will send. Transactional rollback of the ring-buffer read cursor
 * keeps fragmented delivery safe.
 *
 * Returns:
 *   0   - request fully parsed, `pr` filled.
 *   1   - headers incomplete; `rb` is unchanged (rolled back).
 */
int el_prepare_response(RingBuffer *rb, int force_close, int *keep_alive_out, PendingResponse *pr)
{
    if (ring_buffer_is_empty(rb)) {
        return 1;
}

    memset(pr, 0, sizeof(*pr));
    pr->body_fd = -1; /* 0 is a valid descriptor; the sentinel is -1 */
    next_request_id(pr->request_id, sizeof(pr->request_id));
    clock_gettime(CLOCK_MONOTONIC, &pr->started);

    size_t snap_tail = rb->tail;
    size_t snap_size = rb->size;

    char line_buf[MAX_HEADER_LEN];
    size_t line_len = 0;
    HTTPRequest req;
    memset(&req, 0, sizeof(req));
    req.version_minor = -1;
    req.keep_alive = 0; /* closed until a supported version is seen */

    /* --- 1. Request line (skip a bounded run of leading empty lines) ----- */
    int empty_skipped = 0;
    RingLineResult lr;
    for (;;) {
        lr = ring_buffer_readline(rb, line_buf, sizeof(line_buf), &line_len);
        if (lr == RING_LINE_NONE) {
            rb->tail = snap_tail;
            rb->size = snap_size;
            return 1; /* NEED_DATA */
        }
        if (lr == RING_LINE_TOO_LONG) {
            copy_cstr(pr->path, sizeof(pr->path), "/");
            emit_status(pr, 414, 0, 1, NULL, "Request line too long");
            *keep_alive_out = 0;
            return 0;
        }
        if (line_len == 0) {
            if (++empty_skipped > MAX_LEADING_EMPTY_LINES) {
                emit_status(pr, 400, 0, 1, NULL, "Malformed request syntax");
                *keep_alive_out = 0;
                return 0;
            }
            continue;
        }
        break;
    }

    int status = parse_request_line(line_buf, line_len, &req);
    if (status == 0) {
        status = parse_target(&req);
}

    copy_cstr(pr->method, sizeof(pr->method), req.method);
    copy_cstr(pr->path, sizeof(pr->path), req.path);

    if (status != 0) {
        emit_status(pr, status, req.keep_alive, 1, NULL,
                    status == 505 ? "HTTP version not supported"
                                  : "Malformed request syntax");
        *keep_alive_out = 0;
        return 0;
    }

    /* --- 2. Headers ------------------------------------------------------ */
    int header_count = 0;
    for (;;) {
        lr = ring_buffer_readline(rb, line_buf, sizeof(line_buf), &line_len);
        if (lr == RING_LINE_NONE) {
            rb->tail = snap_tail;
            rb->size = snap_size;
            return 1; /* NEED_DATA */
        }
        if (lr == RING_LINE_TOO_LONG) {
            emit_status(pr, 431, 0, 1, NULL, "Request header too large");
            *keep_alive_out = 0;
            return 0;
        }
        if (line_len == 0) {
            break; /* end of header section */
}

        if (line_buf[0] == ' ' || line_buf[0] == '\t') {
            emit_status(pr, 400, 0, 1, NULL, "Obsolete line folding is not allowed");
            *keep_alive_out = 0;
            return 0;
        }
        if (++header_count > MAX_HEADERS) {
            emit_status(pr, 431, 0, 1, NULL, "Too many header fields");
            *keep_alive_out = 0;
            return 0;
        }
        status = parse_header_line(line_buf, line_len, &req);
        if (status != 0) {
            emit_status(pr, status, req.keep_alive, 1, NULL,
                        "Malformed header field");
            *keep_alive_out = 0;
            return 0;
        }
    }
    /* --- Transaction committed ------------------------------------------- */

    *keep_alive_out = req.keep_alive;

    /* --- 3. Framing rules (no request smuggling) ------------------------- */
    if (req.has_transfer_encoding) {
        if (req.has_content_length) {
            emit_status(pr, 400, 0, 1, NULL,
                        "Content-Length conflicts with Transfer-Encoding");
        } else {
            emit_status(pr, 501, 0, 1, NULL,
                        "Transfer-Encoding is not supported");
        }
        *keep_alive_out = 0;
        return 0;
    }

    if (req.expect_unsupported) {
        emit_status(pr, 417, 0, 1, NULL, "Unsupported expectation");
        *keep_alive_out = 0;
        return 0;
    }

    if (req.version_minor == 1 && !req.has_host) {
        emit_status(pr, 400, 0, 1, NULL, "HTTP/1.1 requires a Host header");
        *keep_alive_out = 0;
        return 0;
    }

    if (req.asterisk && method_class(req.method) != METHOD_OPTIONS) {
        emit_status(pr, 400, 0, 1, NULL,
                    "Asterisk request-target is valid only for OPTIONS");
        *keep_alive_out = 0;
        return 0;
    }

    /* --- 4. Method dispatch ---------------------------------------------- */
    int mclass = method_class(req.method);

    if (mclass == METHOD_UNKNOWN) {
        emit_status(pr, 501, 0, 1, NULL, "HTTP method not supported");
        *keep_alive_out = 0;
        return 0;
    }

    if (mclass == METHOD_OPTIONS) {
        format_simple(pr, 204, req.keep_alive, 0, ALLOW_VALUE, NULL, 0);
        return 0;
    }

    if (mclass == METHOD_POST) {
        /* No dynamic content: the method is recognized, just not allowed. */
        emit_status(pr, 405, req.keep_alive, 1, ALLOW_VALUE,
                    "Method not allowed for this resource");
        return 0;
    }

    /* GET / HEAD: a request body would desynchronize the connection. */
    if (req.has_content_length && req.content_length > 0) {
        emit_status(pr, 400, 0, 1, NULL,
                    "Request body is not supported for this method");
        *keep_alive_out = 0;
        return 0;
    }

    req.is_head = (mclass == METHOD_HEAD);

    /* Phase 5: observability endpoints take precedence over document-root
     * resolution while enabled (an operator must keep the configured paths
     * clear of real assets). */
    if (g_obs_enabled) {
        if (path_is(req.path, g_metrics_path)) {
            serve_metrics(&req, force_close, pr);
            return 0;
        }
        if (path_is(req.path, g_health_path)) {
            serve_health(&req, force_close, pr);
            return 0;
        }
        if (path_is(req.path, g_readiness_path)) {
            serve_readiness(&req, force_close, pr);
            return 0;
        }
    }

    /* Legacy fixed-path aliases keep the Phase 1 cached behavior; everything
     * else is resolved against the document root. */
    if (strcmp(req.path, "/home") == 0) {
        select_asset_response(&req, &home_asset, DEFAULT_CONTENT_TYPE,
                              force_close, pr);
        return 0;
    }
    if (strcmp(req.path, "/hello") == 0) {
        select_asset_response(&req, &hello_asset, DEFAULT_CONTENT_TYPE,
                              force_close, pr);
        return 0;
    }

    serve_document_root(&req, force_close, pr);
    return 0;
}
