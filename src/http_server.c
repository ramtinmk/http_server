#include "http_server.h"
#include "ring_buffer.h"
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <zlib.h> 

// --- Configuration ---
#define IDLE_TIMEOUT_SEC 60
#define READ_BUFFER_SIZE 8192

// Error Templates
const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");
const char *PAYLOAD_TOO_LARGE_413 = ERROR_TEMPLATE("413 Payload Too Large", "Request entity too large");
const char *HEADER_FIELDS_TOO_LARGE_431 = ERROR_TEMPLATE("431 Request Header Fields Too Large", "Too many headers");

const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

// --- Helper Functions ---

// Reliable send helper that loops until full buffer is sent or an error occurs
static ssize_t send_all(int sockfd, const void *buf, size_t len) {
    size_t total_sent = 0;
    const char *ptr = (const char *)buf;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) break;
        total_sent += sent;
    }
    return (ssize_t)total_sent;
}

// Reliable scatter-gather send that loops until all iovecs are sent
static ssize_t sendmsg_all(int sockfd, struct iovec *iov, int iovcnt) {
    while (iovcnt > 0) {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = iovcnt;

        ssize_t sent = sendmsg(sockfd, &msg, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) return 0;

        size_t rem = (size_t)sent;
        while (iovcnt > 0 && rem >= iov->iov_len) {
            rem -= iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0 && rem > 0) {
            iov->iov_base = (char *)iov->iov_base + rem;
            iov->iov_len -= rem;
        }
    }
    return 0;
}

// Wrapper to send data without crashing on SIGPIPE
ssize_t send_data(int sockfd, const void *buf, size_t len) {
    return send_all(sockfd, buf, len);
}

void send_error_response(int client_socket, const char *response) {
    if (!response) return;
    send_all(client_socket, response, strlen(response));
}

// Efficiently set timeout using kernel socket options
void set_socket_timeout(int sockfd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        if (errno != EBADF && errno != ENOTSOCK) {
            perror("setsockopt timeout");
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
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// --- Parsing Logic ---

static void parse_request_line(char *line, HTTPRequest *req) {
    if (!line || !req) return;
    
    char *method_end = strchr(line, ' ');
    if (!method_end) return;
    *method_end = '\0';
    
    strncpy(req->method, line, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';
    
    char *path_start = method_end + 1;
    while (*path_start == ' ') path_start++;

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
        while (*version == ' ') version++;
        if (strncasecmp(version, "HTTP/1.0", 8) == 0) {
            req->keep_alive = 0;
        }
    }
}

static void parse_header_line(char *line, HTTPRequest *req) {
    if (!line || !req || req->header_count >= MAX_HEADERS) return;

    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = '\0';

    char *name = line;
    while (*name == ' ' || *name == '\t') name++;
    
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') value++; // Trim leading

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
        if (strstr(value, "gzip")) req->accepts_gzip = 1;
    }
    if (strcasecmp(name, "Connection") == 0) {
        if (strcasecmp(value, "close") == 0) req->keep_alive = 0;
        else if (strcasecmp(value, "keep-alive") == 0) req->keep_alive = 1;
    }
}

int method_is_supported(const char *method) {
    for (int i = 0; i < SUPPORTED_METHOD_COUNT; i++) {
        if (strcmp(method, SUPPORTED_METHODS[i]) == 0) return 1;
    }
    return 0;
}

void print_http_request(const HTTPRequest *req) {
    if (!req) return;
    printf("HTTPRequest: %s %s (keep_alive=%d, gzip=%d, headers=%d)\n",
           req->method, req->path, req->keep_alive, req->accepts_gzip, req->header_count);
    for (int i = 0; i < req->header_count; i++) {
        printf("  %s: %s\n", req->headers[i][0], req->headers[i][1]);
    }
}

// --- Core Request Processor ---

static ProcessResult process_single_request(int client_socket, RingBuffer *rb, int *keep_alive) {
    if (ring_buffer_is_empty(rb)) return REQ_NEED_DATA;

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

        if (line[0] == '\0') break; // Empty line = End of Headers

        if (req.header_count >= MAX_HEADERS) {
            send_error_response(client_socket, HEADER_FIELDS_TOO_LARGE_431);
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
        return REQ_FATAL_ERROR;
    }

    // Map Path
    char filepath[1024];
    if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/home") == 0) {
        snprintf(filepath, sizeof(filepath), "home.html");
    } else if (strcmp(req.path, "/hello") == 0) {
        snprintf(filepath, sizeof(filepath), "hello.html");
    } else {
        send_error_response(client_socket, NOT_FOUND_404);
        return REQ_OK; // 404 is a valid HTTP response, keep connection alive
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) send_error_response(client_socket, NOT_FOUND_404);
        else send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Error", "File Access Error"));
        return REQ_OK;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        send_error_response(client_socket, NOT_FOUND_404);
        return REQ_OK;
    }
    long file_size = st.st_size;
    int use_gzip = req.accepts_gzip && (strcmp(req.method, "HEAD") != 0);

    // 4. Send Response Headers
    char header_buf[1024];
    int offset = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 200 OK\r\n"
        "Server: SimpleHTTPServer/1.0\r\n"
        "Connection: %s\r\n"
        "Content-Type: text/html\r\n",
        (*keep_alive) ? "keep-alive" : "close"
    );

    if (use_gzip) {
        offset += snprintf(header_buf + offset, sizeof(header_buf) - offset, 
            "Content-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n");
    } else {
        offset += snprintf(header_buf + offset, sizeof(header_buf) - offset, 
            "Content-Length: %ld\r\n\r\n", file_size);
    }

    if (send_data(client_socket, header_buf, strlen(header_buf)) < 0) {
        close(fd);
        return REQ_CLIENT_CLOSED;
    }

    // 5. Send Body
    if (strcmp(req.method, "HEAD") == 0) {
        close(fd);
        return REQ_OK;
    }

    if (use_gzip) {
        // --- GZIP STREAMING ---
        unsigned char in[ZLIB_CHUNK_SIZE];
        unsigned char out[ZLIB_CHUNK_SIZE];
        z_stream z;
        memset(&z, 0, sizeof(z));
        
        if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            close(fd);
            return REQ_FATAL_ERROR;
        }

        int flush;
        int z_ret;
        do {
            ssize_t r = read(fd, in, sizeof(in));
            if (r < 0) {
                deflateEnd(&z);
                close(fd);
                return REQ_FATAL_ERROR;
            }
            
            flush = (r == 0) ? Z_FINISH : Z_NO_FLUSH;
            z.avail_in = (uInt)r;
            z.next_in = in;

            do {
                z.avail_out = sizeof(out);
                z.next_out = out;
                z_ret = deflate(&z, flush);
                if (z_ret == Z_STREAM_ERROR) {
                    deflateEnd(&z);
                    close(fd);
                    return REQ_FATAL_ERROR;
                }
                
                size_t have = sizeof(out) - z.avail_out;
                if (have > 0) {
                    char chunk_head[32];
                    int head_len = snprintf(chunk_head, sizeof(chunk_head), "%zx\r\n", have);

                    struct iovec iov[3];
                    iov[0].iov_base = chunk_head;
                    iov[0].iov_len = head_len;
                    iov[1].iov_base = (void *)out;
                    iov[1].iov_len = have;
                    iov[2].iov_base = (void *)"\r\n";
                    iov[2].iov_len = 2;

                    if (sendmsg_all(client_socket, iov, 3) < 0) {
                        deflateEnd(&z);
                        close(fd);
                        return REQ_CLIENT_CLOSED;
                    }
                }
            } while (z.avail_out == 0);
        } while (flush != Z_FINISH);
        
        deflateEnd(&z);
        if (send_all(client_socket, "0\r\n\r\n", 5) < 0) {
            close(fd);
            return REQ_CLIENT_CLOSED;
        }
    } else {
        // --- SENDFILE (Zero Copy) ---
        off_t off = 0;
        while (off < file_size) {
            ssize_t sent = sendfile(client_socket, fd, &off, file_size - off);
            if (sent < 0) {
                close(fd);
                return (errno == EPIPE || errno == ECONNRESET) ? REQ_CLIENT_CLOSED : REQ_FATAL_ERROR;
            }
            if (sent == 0) break;
        }
    }

    close(fd);
    return REQ_OK;
}

// --- Main Thread/Loop ---

void *worker_thread_function(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        Task *task = get_task_from_queue(pool);
        if (!task) break;

        if (task->client_socket >= 0) {
            handle_client(task->client_socket, pool->buffer_pool);
            close(task->client_socket);
        }
        
        task_free(pool->task_pool, task);
    }
    pthread_exit(NULL);
}

void handle_client(int client_socket, BufferPool *bp) {
    if (client_socket < 0 || !bp) return;

    RingBuffer *rb = buffer_acquire(bp);
    if (!rb) {
        fprintf(stderr, "Server overloaded: No buffers available.\n");
        return;
    }

    // Set timeout
    set_socket_timeout(client_socket, IDLE_TIMEOUT_SEC);

    int keep_alive = 1;
    char read_buffer[READ_BUFFER_SIZE];

    while (keep_alive) {
        // 1. Process Pipelined Requests
        // Loop as long as we have valid requests in the buffer
        ProcessResult res;
        do {
            res = process_single_request(client_socket, rb, &keep_alive);
            
            if (res == REQ_FATAL_ERROR || res == REQ_CLIENT_CLOSED) {
                keep_alive = 0;
                break;
            }
            // If REQ_OK, we loop again to see if another request is waiting
        } while (res == REQ_OK && !ring_buffer_is_empty(rb));

        if (!keep_alive) break;

        // 2. Read More Data
        ssize_t bytes = recv(client_socket, read_buffer, sizeof(read_buffer), 0);

        if (bytes > 0) {
            size_t written = ring_buffer_write(rb, read_buffer, bytes);
            if (written < (size_t)bytes) {
                // Buffer overflow or OOM
                send_error_response(client_socket, PAYLOAD_TOO_LARGE_413);
                keep_alive = 0;
            }
        } else if (bytes == 0) {
            // Client closed gracefully
            keep_alive = 0;
        } else {
            // Error or Timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout
                keep_alive = 0; 
            } else if (errno != EINTR && errno != EBADF && errno != ENOTSOCK && errno != ECONNRESET) {
                perror("recv");
                keep_alive = 0;
            } else {
                keep_alive = 0;
            }
        }
    }

    buffer_release(bp, rb);
}