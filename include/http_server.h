#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "ring_buffer.h"
#include "server_config.h"
#include "config.h"

/* Shared listening parameters (socket setup and startup diagnostics). */
#define PORT 8081
#ifndef BACKLOG
#define BACKLOG 1024
#endif

/*
 * Maximum size of a generated response header block, including an optional
 * small (error) body embedded after the headers. Large enough for the full
 * validator/range/negotiation header set; a response that would not fit is
 * replaced by a minimal 500.
 */
#define PR_HEADER_BUF_SIZE 512

/*
 * Response descriptor for the event-loop path.
 *
 * `header` points either into startup-cached or static-string (literal) memory
 * or is NULL, meaning the generated block in `header_buf` is authoritative.
 * `body` points into cached memory; the event loop must NEVER free it. body may
 * be NULL when body_len == 0.
 *
 * The one exception is `owned_body`: a multipart/byteranges body is assembled
 * on the heap and the response owns it. When `owned_body` is non-NULL it aliases
 * `body`, and the event loop frees it after the response is fully sent or when
 * the connection is closed with the response still queued. Every other body is
 * borrowed from cached/static memory and must not be freed.
 *
 * header_buf is part of the struct and therefore travels with the by-value
 * copy into the pipeline queue; a response that wants dynamic headers sets
 * `header = NULL` and fills header_buf. Never set `header` to point at
 * `header_buf` itself: after the queue copy it would dangle.
 */
typedef struct {
    const char          *header;       /* Header block, or NULL for header_buf. */
    size_t               header_len;   /* Length of header block in bytes.   */
    const unsigned char *body;         /* Body bytes (may be NULL).          */
    size_t               body_len;     /* Length of body in bytes.           */
    unsigned char       *owned_body;   /* Heap body this response owns, or NULL. */
    int                  body_fd;      /* Streamed file fd (owned), or -1.    */
    off_t                body_file_off;/* Starting offset for the stream.    */
    void                *cache_entry;  /* Pinned FileCache entry, or NULL.   */
    int                  is_head;      /* HEAD request: skip body send.      */
    int                  force_close;  /* Close connection after this resp.  */
    int                  status;       /* HTTP status code (for metrics).    */

    /* Access-log metadata, filled at parse time. */
    char                 method[16];
    char                 path[256];
    struct timespec      started;      /* CLOCK_MONOTONIC, request parse time. */

    /* Generated header block used when `header` is NULL. */
    char                 header_buf[PR_HEADER_BUF_SIZE];
} PendingResponse;

/*
 * Parse one HTTP request from `rb` and populate `pr` with the response
 * descriptor that the event loop can enqueue and send without blocking.
 *
 * On return:
 *   0  — one request fully parsed; `pr` is filled; `*keep_alive_out` is set.
 *   1  — headers are incomplete; caller must buffer more data and retry.
 *         `rb` is left unchanged (transaction rolled back).
 */
int el_prepare_response(RingBuffer *rb, int force_close, int *keep_alive_out, PendingResponse *pr);

int create_server_socket(const ServerConfig *cfg, int reuseport);

/*
 * Create and bind the optional TLS listener on `cfg->tls_port`. Same socket
 * options as create_server_socket(); `reuseport` is set when several event
 * loops each bind their own TLS listener. Returns the fd, or -1 on error.
 */
int create_tls_server_socket(const ServerConfig *cfg, int reuseport);

/*
 * Initialize static serving: load and precompress the legacy fixed-path assets
 * (`root/home.html`/`root/hello.html`) and configure the Phase 2 document root, MIME map,
 * path resolver, and bounded representation cache from `cfg`. Returns 0 on
 * success, -1 when an asset or the document root cannot be loaded.
 */
int initialize_static_responses(const ServerConfig *cfg);

/* Release resolver/cache resources at shutdown. */
void shutdown_static_responses(void);

/* Current representation-cache byte count and entry count (metrics sampler). */
void http_server_cache_stats(long *bytes, long *entries);

#endif /* HTTP_SERVER_H */
