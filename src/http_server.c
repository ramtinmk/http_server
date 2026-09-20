#include "http_server.h"
#include "ring_buffer.h"
#include "metrics.h"
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <zlib.h> 

// --- Configuration (limits come from server_config.h via http_server.h) ---
#define READ_BUFFER_SIZE 8192
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

// Error Templates
const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");
const char *PAYLOAD_TOO_LARGE_413 = ERROR_TEMPLATE("413 Payload Too Large", "Request entity too large");
const char *HEADER_FIELDS_TOO_LARGE_431 = ERROR_TEMPLATE("431 Request Header Fields Too Large", "Too many headers");

const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

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

// --- Helper Functions ---

// Reliable send helper that loops until full buffer is sent or an error occurs
static ssize_t send_all(int sockfd, const void *buf, size_t len) {
    size_t total_sent = 0;
    const char *ptr = (const char *)buf;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) { continue;
}
            return -1;
        }
        if (sent == 0) { break;
}
        total_sent += sent;
    }
    return (ssize_t)total_sent;
}


// Wrapper to send data without crashing on SIGPIPE
ssize_t send_data(int sockfd, const void *buf, size_t len) {
    return send_all(sockfd, buf, len);
}

void send_error_response(int client_socket, const char *response) {
    if (!response) { return;
}
    send_all(client_socket, response, strlen(response));
}

// Set receive timeout (SO_RCVTIMEO) on a socket.
void set_socket_timeout(int sockfd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        if (errno != EBADF && errno != ENOTSOCK) {
            perror("setsockopt SO_RCVTIMEO");
        }
    }
}

// Set send timeout (SO_SNDTIMEO) on a socket.  Protects against slow readers
// blocking a worker thread while writing a response.
static void set_socket_write_timeout(int sockfd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv) < 0) {
        if (errno != EBADF && errno != ENOTSOCK) {
            perror("setsockopt SO_SNDTIMEO");
        }
    }
}

int create_server_socket(void) {
    // CRITICAL: Ignore SIGPIPE globally. 
    // Otherwise, writing to a closed client crashes the server.
    signal(SIGPIPE, SIG_IGN);

    int server_socket;
    struct sockaddr_in server_addr;

    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Mark the listening socket close-on-exec so child processes (if any) do
     * not accidentally inherit it. */
    {
        int fl = fcntl(server_socket, F_GETFD);
        if (fl >= 0) { fcntl(server_socket, F_SETFD, fl | FD_CLOEXEC);
}
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt reuseaddr");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_socket;
}

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {;
}
}

// --- Parsing Logic ---

static void parse_request_line(char *line, HTTPRequest *req) {
    if (!line || !req) { return;
}
    
    char *method_end = strchr(line, ' ');
    if (!method_end) { return;
}
    *method_end = '\0';
    
    strncpy(req->method, line, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';
    
    char *path_start = method_end + 1;
    while (*path_start == ' ') { path_start++;
}

    char *path_end = strchr(path_start, ' ');
    if (!path_end) {
        // HTTP/0.9 or missing version, assume rest is path
        strncpy(req->path, path_start, sizeof(req->path) - 1);
        req->path[sizeof(req->path) - 1] = '\0';
    } else {
        *path_end = '\0';
        strncpy(req->path, path_start, sizeof(req->path) - 1);
        req->path[sizeof(req->path) - 1] = '\0';

        char *version = path_end + 1;
        while (*version == ' ') { version++;
}
        if (strncasecmp(version, "HTTP/1.0", 8) == 0) {
            req->keep_alive = 0;
        }
    }
}

static int gzip_is_accepted(const char *value) {
    if (!value) { return 0;
}
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", value);

    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ",", &saveptr);
         item;
         item = strtok_r(NULL, ",", &saveptr)) {
        while (*item == ' ' || *item == '\t') { item++;
}
        char *end = item + strlen(item);
        while (end > item && (end[-1] == ' ' || end[-1] == '\t')) { *--end = '\0';
}

        char *parameters = strchr(item, ';');
        if (parameters) { *parameters++ = '\0';
}
        if (strcasecmp(item, "gzip") != 0) { continue;
}

        int accepted = 1;
        if (parameters) {
            char *parameter_save = NULL;
            for (char *parameter = strtok_r(parameters, ";", &parameter_save);
                 parameter;
                 parameter = strtok_r(NULL, ";", &parameter_save)) {
                while (*parameter == ' ' || *parameter == '\t') { parameter++;
}
                char *equals = strchr(parameter, '=');
                if (!equals) { continue;
}
                char *name_end = equals;
                while (name_end > parameter &&
                       (name_end[-1] == ' ' || name_end[-1] == '\t')) {
                    name_end--;
                }
                *name_end = '\0';
                if (strcasecmp(parameter, "q") != 0) { continue;
}

                char *qvalue = equals + 1;
                while (*qvalue == ' ' || *qvalue == '\t') { qvalue++;
}
                char *qend = NULL;
                double quality = strtod(qvalue, &qend);
                while (qend && (*qend == ' ' || *qend == '\t')) { qend++;
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
    if (!line || !req || req->header_count >= MAX_HEADERS) { return;
}

    char *colon = strchr(line, ':');
    if (!colon) { return;
}
    *colon = '\0';

    char *name = line;
    while (*name == ' ' || *name == '\t') { name++;
}
    
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') { value++; // Trim leading
}

    // Trim trailing (CR/LF/whitespace)
    size_t len = strlen(value);
    while (len > 0 && (value[len-1] == '\r' || value[len-1] == '\n' || value[len-1] == ' ' || value[len-1] == '\t')) {
        value[len-1] = '\0';
        len--;
    }

    strncpy(req->headers[req->header_count][0], name, sizeof(req->headers[0][0]) - 1);
    req->headers[req->header_count][0][sizeof(req->headers[0][0]) - 1] = '\0';

    strncpy(req->headers[req->header_count][1], value, sizeof(req->headers[0][1]) - 1);
    req->headers[req->header_count][1][sizeof(req->headers[0][1]) - 1] = '\0';

    req->header_count++;

    // Logic Hooks
    if (strcasecmp(name, "Accept-Encoding") == 0) {
        req->accepts_gzip = gzip_is_accepted(value);
    }
    if (strcasecmp(name, "Connection") == 0) {
        if (strcasecmp(value, "close") == 0) { req->keep_alive = 0;
        } else if (strcasecmp(value, "keep-alive") == 0) { req->keep_alive = 1;
}
    }
}

int method_is_supported(const char *method) {
    for (int i = 0; i < SUPPORTED_METHOD_COUNT; i++) {
        if (strcmp(method, SUPPORTED_METHODS[i]) == 0) { return 1;
}
    }
    return 0;
}

void print_http_request(const HTTPRequest *req) {
    if (!req) { return;
}
    printf("HTTPRequest: %s %s (keep_alive=%d, gzip=%d, headers=%d)\n",
           req->method, req->path, req->keep_alive, req->accepts_gzip, req->header_count);
    for (int i = 0; i < req->header_count; i++) {
        printf("  %s: %s\n", req->headers[i][0], req->headers[i][1]);
    }
}

// --- Core Request Processor ---

static ProcessResult process_single_request(int client_socket, RingBuffer *rb, int *keep_alive) {
    if (ring_buffer_is_empty(rb)) { return REQ_NEED_DATA;
}

    // --- TRANSACTION START ---
    // Save state. If we fail to find a full header set, we ROLLBACK.
    size_t rb_snapshot_tail = rb->tail;
    size_t rb_snapshot_size = rb->size;

    char line_buf[MAX_HEADER_LEN];
    HTTPRequest req;
    memset(&req, 0, sizeof(HTTPRequest));
    req.keep_alive = 1; // Default for HTTP/1.1

    // 1. Parse Request Line
    char *line = ring_buffer_readline(rb, line_buf, sizeof(line_buf));
    if (!line) {
        // Rollback: Not a full line yet
        rb->tail = rb_snapshot_tail;
        rb->size = rb_snapshot_size;
        return REQ_NEED_DATA;
    }

    parse_request_line(line, &req);
    if (strlen(req.method) == 0 || strlen(req.path) == 0) {
        send_error_response(client_socket, BAD_REQUEST_400);
        metrics_response(400);
        return REQ_FATAL_ERROR;
    }

    // 2. Parse Headers
    while (1) {
        line = ring_buffer_readline(rb, line_buf, sizeof(line_buf));
        if (!line) {
            // Incomplete headers. Rollback entire transaction.
            rb->tail = rb_snapshot_tail;
            rb->size = rb_snapshot_size;
            return REQ_NEED_DATA;
        }

        if (line[0] == '\0') { break; // Empty line = End of Headers
}

        if (req.header_count >= MAX_HEADERS) {
            send_error_response(client_socket, HEADER_FIELDS_TOO_LARGE_431);
            metrics_response(431);
            return REQ_FATAL_ERROR;
        }
        parse_header_line(line, &req);
    }
    // --- TRANSACTION COMMITTED ---
    // At this point, we have consumed the request from the ring buffer.

    // 3. Logic Execution
    *keep_alive = req.keep_alive;

    if (!method_is_supported(req.method)) {
        send_error_response(client_socket, NOT_IMPLEMENTED_501);
        metrics_response(501);
        return REQ_FATAL_ERROR;
    }

    // Select a server-lifetime cached representation.
    CachedResponse *response = NULL;
    if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/home") == 0) {
        response = req.accepts_gzip ? &home_asset.gzip : &home_asset.plain;
    } else if (strcmp(req.path, "/hello") == 0) {
        response = req.accepts_gzip ? &hello_asset.gzip : &hello_asset.plain;
    } else {
        send_error_response(client_socket, NOT_FOUND_404);
        metrics_response(404);
        return REQ_OK; // 404 is a valid HTTP response, keep connection alive
    }

    if (!response || !response->body) {
        send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Error", "Static response unavailable"));
        metrics_request_failed();
        metrics_response(500);
        return REQ_OK;
    }

    // Headers, body bytes, and lengths are all prepared during startup.
    int keep_alive_index = (*keep_alive) ? 1 : 0;
    if (send_data(client_socket, response->headers[keep_alive_index],
                  response->headers_len[keep_alive_index]) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            metrics_write_timeout();
        }
        metrics_request_failed();
        return REQ_CLIENT_CLOSED;
    }
    metrics_response(200);

    if (strcmp(req.method, "HEAD") == 0) { return REQ_OK;
}
    if (send_data(client_socket, response->body, response->body_len) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            metrics_write_timeout();
        }
        metrics_request_failed();
        return REQ_CLIENT_CLOSED;
    }
    return REQ_OK;
}

// --- Main Thread/Loop ---

void *worker_thread_function(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        Task *task = get_task_from_queue(pool);
        if (!task) { break;
}

        if (task->client_socket >= 0) {
            metrics_worker_busy();
            handle_client(task->client_socket, pool->buffer_pool);
            close(task->client_socket);
            metrics_active_connection_dec();
            metrics_worker_idle();
        }
        
        task_free(pool->task_pool, task);
    }
    pthread_exit(NULL);
}

void handle_client(int client_socket, BufferPool *bp) {
    if (client_socket < 0 || !bp) { return;
}

    RingBuffer *rb = buffer_acquire(bp);
    if (!rb) {
        /* Buffer pool is strictly bounded; no fallback allocation. */
        metrics_buffer_pool_exhausted();
        return;
    }

    /* Write timeout: prevents a slow receiver from holding this worker while
     * we block inside send().  Applied once at connection start. */
    set_socket_write_timeout(client_socket, WRITE_TIMEOUT_SEC);

    /* Header-read timeout: shorter window for the very first request headers.
     * Switched to the longer idle timeout after the first successful response. */
    set_socket_timeout(client_socket, HEADER_READ_TIMEOUT_SEC);

    int keep_alive    = 1;
    int request_count = 0;   /* total requests served on this connection */
    char read_buffer[READ_BUFFER_SIZE];

    while (keep_alive) {
        /* 1. Drain pipelined requests already sitting in the ring buffer. */
        int pipeline_depth = 0;
        ProcessResult res;
        do {
            res = process_single_request(client_socket, rb, &keep_alive);

            if (res == REQ_FATAL_ERROR || res == REQ_CLIENT_CLOSED) {
                keep_alive = 0;
                break;
            }
            if (res == REQ_OK) {
                request_count++;
                pipeline_depth++;
                /* Enforce per-connection keep-alive request limit. */
                if (request_count >= MAX_KEEPALIVE_REQUESTS) {
                    keep_alive = 0;
                    break;
                }
                /* Enforce pipeline depth: don't process unbounded pipelined
                 * requests from one recv() pass before reading more data. */
                if (pipeline_depth >= MAX_PIPELINE_DEPTH) {
                    break;
                }
            }
        } while (res == REQ_OK && !ring_buffer_is_empty(rb));

        if (!keep_alive) { break;
}

        /* 2. After the first successful response switch to the longer idle
         *    timeout so well-behaved keep-alive clients aren't prematurely
         *    closed. */
        if (request_count > 0) {
            set_socket_timeout(client_socket, IDLE_TIMEOUT_SEC);
        }

        /* 3. Enforce input buffer size limit before accepting more data. */
        if (ring_buffer_get_size(rb) >= MAX_INPUT_BUFFER_BYTES) {
            send_error_response(client_socket, PAYLOAD_TOO_LARGE_413);
            metrics_response(413);
            metrics_input_buffer_limit();
            break;
        }

        /* 4. Read more data from the socket. */
        ssize_t bytes = recv(client_socket, read_buffer, sizeof(read_buffer), 0);

        if (bytes > 0) {
            size_t written = ring_buffer_write(rb, read_buffer, (size_t)bytes);
            if (written < (size_t)bytes) {
                /* Ring buffer could not grow (at its own hard ceiling). */
                send_error_response(client_socket, PAYLOAD_TOO_LARGE_413);
                metrics_response(413);
                metrics_input_buffer_limit();
                break;
            }
        } else if (bytes == 0) {
            /* Clean close from client. */
            keep_alive = 0;
        } else {
            /* Error or timeout. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (request_count == 0) {
                    metrics_header_timeout();
                } else {
                    metrics_idle_timeout();
                }
            }
            keep_alive = 0;
        }
    }

    buffer_release(bp, rb);
}

/* ------------------------------------------------------------------ */
/* Phase 3: event-loop response preparation                             */
/* ------------------------------------------------------------------ */

/*
 * Parse one HTTP request from `rb` and populate `pr` with the cached
 * response the event loop will send.  Uses the same transaction/rollback
 * logic as process_single_request so fragmented delivery is safe.
 *
 * Error responses re-use the compile-time string literals defined at the
 * top of this file (BAD_REQUEST_400, NOT_FOUND_404, ...) as the `header`
 * field with body_len == 0, because the HTML payload is embedded inline.
 * All error responses set force_close = 1 so the event loop closes the
 * connection after delivering them.
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
    HTTPRequest req;
    memset(&req, 0, sizeof(HTTPRequest));
    req.keep_alive = 1; /* HTTP/1.1 default */

    /* --- 1. Parse request line ----------------------------------------- */
    char *line = ring_buffer_readline(rb, line_buf, sizeof(line_buf));
    if (!line) {
        rb->tail = snap_tail;
        rb->size = snap_size;
        return 1; /* NEED_DATA */
    }

    parse_request_line(line, &req);
    if (strlen(req.method) == 0 || strlen(req.path) == 0) {
        /* Malformed request line — consume bytes and return error response. */
        pr->header      = BAD_REQUEST_400;
        pr->header_len  = strlen(BAD_REQUEST_400);
        pr->body        = NULL;
        pr->body_len    = 0;
        pr->is_head     = 0;
        pr->force_close = 1;
        pr->status      = 400;
        return 0;
    }

    /* --- 2. Parse headers ----------------------------------------------- */
    while (1) {
        line = ring_buffer_readline(rb, line_buf, sizeof(line_buf));
        if (!line) {
            /* Incomplete headers: roll back entire transaction. */
            rb->tail = snap_tail;
            rb->size = snap_size;
            return 1; /* NEED_DATA */
        }
        if (line[0] == '\0') { break; } /* Empty line -> end of headers. */

        if (req.header_count >= MAX_HEADERS) {
            pr->header      = HEADER_FIELDS_TOO_LARGE_431;
            pr->header_len  = strlen(HEADER_FIELDS_TOO_LARGE_431);
            pr->body        = NULL;
            pr->body_len    = 0;
            pr->is_head     = 0;
            pr->force_close = 1;
            pr->status      = 431;
            return 0;
        }
        parse_header_line(line, &req);
    }
    /* --- Transaction committed ------------------------------------------- */

    *keep_alive_out = req.keep_alive;

    /* --- 3. Validate method --------------------------------------------- */
    if (!method_is_supported(req.method)) {
        pr->header      = NOT_IMPLEMENTED_501;
        pr->header_len  = strlen(NOT_IMPLEMENTED_501);
        pr->body        = NULL;
        pr->body_len    = 0;
        pr->is_head     = 0;
        pr->force_close = 1;
        pr->status      = 501;
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
        /* 404 — keep connection alive just like the blocking path does. */
        pr->header      = NOT_FOUND_404;
        pr->header_len  = strlen(NOT_FOUND_404);
        pr->body        = NULL;
        pr->body_len    = 0;
        pr->is_head     = 0;
        pr->force_close = 0;
        pr->status      = 404;
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
