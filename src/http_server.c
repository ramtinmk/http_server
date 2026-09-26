#include "http_server.h"
#include "metrics.h"

#include <signal.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
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

/* Response template for parser-generated errors. The body is embedded in the
 * header block, so these are sent as one complete response. */
#define ERROR_TEMPLATE(status, msg) \
    "HTTP/1.1 " status "\r\n" \
    "Content-Type: text/html\r\n" \
    "Connection: close\r\n\r\n" \
    "<html><head><title>" status "</title></head>" \
    "<body><h1>" status "</h1><p>" msg "</p></body></html>\r\n"

/* --- Request model (private to the parser) --- */
typedef struct {
    char method[16];
    char path[1024];
    int header_count;
    int keep_alive;
    int accepts_gzip;
} HTTPRequest;

/* --- Cached static asset (identity and gzip variants) --- */
#define RESPONSE_HEADER_SIZE 512

typedef struct {
    unsigned char *body;
    size_t body_len;
    char headers[2][RESPONSE_HEADER_SIZE];
    size_t headers_len[2];
} CachedResponse;

typedef struct {
    CachedResponse plain;
    CachedResponse gzip;
} StaticAsset;

static StaticAsset home_asset;
static StaticAsset hello_asset;
static int static_responses_initialized;

/* Parser-generated error responses (whole response is a compile-time literal). */
static const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
static const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
static const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");
static const char *REQUEST_URI_TOO_LONG_414 = ERROR_TEMPLATE("414 URI Too Long", "Request line too long");
static const char *HEADER_FIELDS_TOO_LARGE_431 = ERROR_TEMPLATE("431 Request Header Fields Too Large", "Request header too large");

static const char *const SUPPORTED_METHODS[] = {"GET", "HEAD"};

static void free_cached_response(CachedResponse *response) {
    if (!response) { return;
}
    free(response->body);
    memset(response, 0, sizeof(*response));
}

static int read_asset(const char *path, unsigned char **body, size_t *body_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { return -1;
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
        if (count < 0 && errno == EINTR) { continue;
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
    return 0;
}

static int gzip_asset(const unsigned char *input, size_t input_len,
                      unsigned char **output, size_t *output_len) {
    uLong bound = compressBound((uLong)input_len);
    unsigned char *data = malloc(bound ? (size_t)bound : 1);
    if (!data) {
        errno = ENOMEM;
        return -1;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(data);
        errno = EIO;
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

static int prepare_headers(CachedResponse *response, int gzip) {
    for (int keep_alive = 0; keep_alive <= 1; keep_alive++) {
        int written = snprintf(response->headers[keep_alive],
                               sizeof(response->headers[keep_alive]),
                               "HTTP/1.1 200 OK\r\n"
                               "Server: SimpleHTTPServer/1.0\r\n"
                               "Connection: %s\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: %zu\r\n"
                               "%s"
                               "Vary: Accept-Encoding\r\n\r\n",
                               keep_alive ? "keep-alive" : "close",
                               response->body_len,
                               gzip ? "Content-Encoding: gzip\r\n" : "");
        if (written < 0 || (size_t)written >= sizeof(response->headers[keep_alive])) {
            errno = EOVERFLOW;
            return -1;
        }
        response->headers_len[keep_alive] = (size_t)written;
    }
    return 0;
}

static int load_static_asset(const char *path, StaticAsset *asset) {
    memset(asset, 0, sizeof(*asset));
    if (read_asset(path, &asset->plain.body, &asset->plain.body_len) != 0 ||
        gzip_asset(asset->plain.body, asset->plain.body_len,
                   &asset->gzip.body, &asset->gzip.body_len) != 0 ||
        prepare_headers(&asset->plain, 0) != 0 ||
        prepare_headers(&asset->gzip, 1) != 0) {
        free_cached_response(&asset->plain);
        free_cached_response(&asset->gzip);
        return -1;
    }
    return 0;
}

int initialize_static_responses(void) {
    if (static_responses_initialized) { return 0;
}
    if (load_static_asset("home.html", &home_asset) != 0) {
        fprintf(stderr, "Failed to cache home.html: %s\n", strerror(errno));
        return -1;
    }
    if (load_static_asset("hello.html", &hello_asset) != 0) {
        fprintf(stderr, "Failed to cache hello.html: %s\n", strerror(errno));
        free_cached_response(&home_asset.plain);
        free_cached_response(&home_asset.gzip);
        return -1;
    }
    static_responses_initialized = 1;
    return 0;
}

int create_server_socket(int reuseport) {
    /* Ignore SIGPIPE globally: writing to a closed client must not kill the
     * server. */
    signal(SIGPIPE, SIG_IGN);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return -1;
    }

    /* Mark the listening socket close-on-exec so child processes (if any) do
     * not accidentally inherit it. */
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

    /* When multiple event loops are enabled, each loop binds its own listener,
     * so every socket on the port must set SO_REUSEPORT. With a single loop we
     * deliberately do not, so a second accidental instance still fails to bind
     * instead of silently sharing the port. */
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
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_socket);
        return -1;
    }

    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

// --- Parsing Logic ---

/* Advance past leading spaces and tabs. */
static char *skip_ws(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Strip trailing bytes that appear in `set` (space, tab, CR, LF, ...). */
static void rtrim(char *s, const char *set) {
    char *end = s + strlen(s);
    while (end > s && strchr(set, end[-1])) {
        *--end = '\0';
    }
}

static void parse_request_line(char *line, HTTPRequest *req) {
    if (!line || !req) {
        return;
    }

    char *method_end = strchr(line, ' ');
    if (!method_end) {
        return;
    }
    *method_end = '\0';
    snprintf(req->method, sizeof(req->method), "%s", line);

    char *path_start = skip_ws(method_end + 1);
    char *path_end = strchr(path_start, ' ');
    if (path_end) {
        /* Version present: HTTP/1.0 defaults to close (HTTP/1.1 keeps alive). */
        *path_end = '\0';
        if (strncasecmp(skip_ws(path_end + 1), "HTTP/1.0", 8) == 0) {
            req->keep_alive = 0;
        }
    }
    /* Without a version (HTTP/0.9) the rest of the line is the path. */
    snprintf(req->path, sizeof(req->path), "%s", path_start);
}

static int gzip_is_accepted(const char *value) {
    if (!value) {
        return 0;
    }
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", value);

    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ",", &saveptr);
         item;
         item = strtok_r(NULL, ",", &saveptr)) {
        item = skip_ws(item);
        char *parameters = strchr(item, ';');
        if (parameters) {
            *parameters++ = '\0';
        }
        rtrim(item, " \t"); /* "gzip ;q=..." -> "gzip" */
        if (strcasecmp(item, "gzip") != 0) {
            continue;
        }

        int accepted = 1;
        if (parameters) {
            char *parameter_save = NULL;
            for (char *parameter = strtok_r(parameters, ";", &parameter_save);
                 parameter;
                 parameter = strtok_r(NULL, ";", &parameter_save)) {
                parameter = skip_ws(parameter);
                char *equals = strchr(parameter, '=');
                if (!equals) {
                    continue;
                }
                *equals = '\0';
                rtrim(parameter, " \t");
                if (strcasecmp(parameter, "q") != 0) {
                    continue;
                }

                char *qend = NULL;
                char *qvalue = skip_ws(equals + 1);
                double quality = strtod(qvalue, &qend);
                if (qend) {
                    qend = skip_ws(qend);
                }
                if (qend == qvalue || (qend && *qend != '\0') ||
                    quality < 0.0 || quality > 1.0 || quality == 0.0) {
                    accepted = 0;
                }
                break;
            }
        }
        return accepted;
    }
    return 0;
}

static void parse_header_line(char *line, HTTPRequest *req) {
    if (!line || !req || req->header_count >= MAX_HEADERS) {
        return;
    }

    char *colon = strchr(line, ':');
    if (!colon) {
        return;
    }
    *colon = '\0';

    char *name = skip_ws(line);
    char *value = skip_ws(colon + 1);
    rtrim(value, "\r\n \t"); /* trim trailing CR/LF/whitespace */

    req->header_count++;

    if (strcasecmp(name, "Accept-Encoding") == 0) {
        req->accepts_gzip = gzip_is_accepted(value);
    } else if (strcasecmp(name, "Connection") == 0) {
        if (strcasecmp(value, "close") == 0) {
            req->keep_alive = 0;
        } else if (strcasecmp(value, "keep-alive") == 0) {
            req->keep_alive = 1;
        }
    }
}

static int method_is_supported(const char *method) {
    for (size_t i = 0; i < sizeof(SUPPORTED_METHODS) / sizeof(SUPPORTED_METHODS[0]); i++) {
        if (strcmp(method, SUPPORTED_METHODS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase 3: event-loop response preparation                             */
/* ------------------------------------------------------------------ */

/* Point `pr` at a compile-time static response literal (body_len == 0; the
 * HTML payload is embedded in the header block). `force_close` requests that
 * the event loop close the connection after delivering it. */
static void fill_static_response(PendingResponse *pr, const char *response,
                                 int status, int force_close)
{
    pr->header      = response;
    pr->header_len  = strlen(response);
    pr->body        = NULL;
    pr->body_len    = 0;
    pr->is_head     = 0;
    pr->force_close = force_close;
    pr->status      = status;
}

/*
 * Parse one HTTP request from `rb` and populate `pr` with the cached
 * response the event loop will send.  Transactional rollback of the
 * ring-buffer read cursor keeps fragmented delivery safe.
 *
 * Returns:
 *   0   - request fully parsed, `pr` filled.
 *   1   - headers incomplete; `rb` is unchanged (rolled back).
 */
int el_prepare_response(RingBuffer *rb, int force_close, int *keep_alive_out, PendingResponse *pr)
{
    if (ring_buffer_is_empty(rb)) { return 1; }

    /* Transaction: save ring-buffer read state for rollback on NEED_DATA. */
    size_t snap_tail = rb->tail;
    size_t snap_size = rb->size;

    char line_buf[MAX_HEADER_LEN];
    size_t line_len = 0;
    HTTPRequest req;
    memset(&req, 0, sizeof(HTTPRequest));
    req.keep_alive = 1; /* HTTP/1.1 default */

    /* --- 1. Parse request line ----------------------------------------- */
    RingLineResult lr = ring_buffer_readline(rb, line_buf, sizeof(line_buf), &line_len);
    if (lr == RING_LINE_NONE) {
        rb->tail = snap_tail;
        rb->size = snap_size;
        return 1; /* NEED_DATA */
    }
    if (lr == RING_LINE_TOO_LONG) {
        /* Request line exceeds the parser limit; consume and reject. */
        fill_static_response(pr, REQUEST_URI_TOO_LONG_414, 414, 1);
        return 0;
    }

    parse_request_line(line_buf, &req);
    if (strlen(req.method) == 0 || strlen(req.path) == 0) {
        /* Malformed request line — consume bytes and return error response. */
        fill_static_response(pr, BAD_REQUEST_400, 400, 1);
        return 0;
    }

    /* --- 2. Parse headers ----------------------------------------------- */
    while (1) {
        lr = ring_buffer_readline(rb, line_buf, sizeof(line_buf), &line_len);
        if (lr == RING_LINE_NONE) {
            /* Incomplete headers: roll back entire transaction. */
            rb->tail = snap_tail;
            rb->size = snap_size;
            return 1; /* NEED_DATA */
        }
        if (lr == RING_LINE_TOO_LONG) {
            /* Header line exceeds the parser limit; consume and reject. */
            fill_static_response(pr, HEADER_FIELDS_TOO_LARGE_431, 431, 1);
            return 0;
        }
        if (line_buf[0] == '\0') { break; } /* Empty line -> end of headers. */

        if (req.header_count >= MAX_HEADERS) {
            fill_static_response(pr, HEADER_FIELDS_TOO_LARGE_431, 431, 1);
            return 0;
        }
        parse_header_line(line_buf, &req);
    }
    /* --- Transaction committed ------------------------------------------- */

    *keep_alive_out = req.keep_alive;

    /* --- 3. Validate method --------------------------------------------- */
    if (!method_is_supported(req.method)) {
        fill_static_response(pr, NOT_IMPLEMENTED_501, 501, 1);
        return 0;
    }

    /* --- 4. Select cached response -------------------------------------- */
    CachedResponse *resp = NULL;
    if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/home") == 0) {
        resp = req.accepts_gzip ? &home_asset.gzip : &home_asset.plain;
    } else if (strcmp(req.path, "/hello") == 0) {
        resp = req.accepts_gzip ? &hello_asset.gzip : &hello_asset.plain;
    }

    if (!resp || !resp->body) {
        /* Unknown path: 404, keep-alive preserved. */
        fill_static_response(pr, NOT_FOUND_404, 404, 0);
        return 0;
    }

    /* --- 5. 200 OK ------------------------------------------------------- */
    /* A forced-close response (keep-alive request limit reached) must
     * advertise Connection: close so the client does not reuse the socket. */
    int ka_idx = force_close ? 0 : (req.keep_alive ? 1 : 0);
    pr->header          = resp->headers[ka_idx];
    pr->header_len      = resp->headers_len[ka_idx];
    pr->body            = resp->body;
    pr->body_len        = resp->body_len;
    pr->is_head         = (strcmp(req.method, "HEAD") == 0);
    pr->force_close     = force_close;
    pr->status          = 200;
    return 0;
}
