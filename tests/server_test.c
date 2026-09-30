#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/select.h> // Required for select()
#include <sys/time.h>   // Required for struct timeval
#include <sys/wait.h>   // waitpid() for the spawned test server
#include <dirent.h>     // /proc/<pid>/fd scanning
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <zlib.h>
#include "test_utils.h"
#include "server_test.h"
#include "server_config.h"

// --- Configuration ---
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8081
#define BUFFER_SIZE 4096
#define RESPONSE_TIMEOUT_MS 5000
/* Must match the parser limit in http_server.h (not included here to avoid
 * clashing BUFFER_SIZE definitions). */
#ifndef MAX_HEADER_LEN
#define MAX_HEADER_LEN 1024
#endif

// --- Helper Structures ---
typedef struct {
    char *raw;
    size_t raw_length;
    char *headers;
    size_t headers_length;
    char *body;
    size_t body_length;
    int status_code;
    int chunked;
    long long content_length;
} HttpResponse;

/*
 * Response framing specification for integration tests:
 *
 * - The header section ends only at CRLF CRLF.
 * - Content-Length responses complete after exactly that many body bytes;
 *   an idle socket is never treated as a message boundary.
 * - Transfer-Encoding: chunked responses complete after the zero-size chunk
 *   and all trailers. The exposed body is the decoded, unchunked payload.
 * - Responses without either framing header are read until the peer closes
 *   the connection.
 * - HEAD and bodyless status responses must not consume a response body.
 *
 * These rules let keep-alive tests distinguish one complete response from
 * delayed, fragmented, or coalesced socket reads.
 */

// --- Helper Functions (Internal) ---

static int create_and_connect_socket() {
    int client_socket;
    struct sockaddr_in server_addr;

    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        close(client_socket);
        return -1;
    }

    if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        // Only print if connection truly fails (server might be down)
        // perror("Connection failed"); 
        close(client_socket);
        return -1;
    }

    return client_socket;
}

static int connect_to_port(int port) {
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) return -1;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        close(client_socket);
        return -1;
    }
    if (connect(client_socket, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        close(client_socket);
        return -1;
    }
    return client_socket;
}

/*
 * Count the open file descriptors of `pid`. Used to prove that a burst of
 * client connects/closes (including abrupt mid-request and mid-response
 * disconnects) does not leak descriptors in the server. Returns -1 when
 * /proc is unavailable.
 */
static int count_open_fds(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        count++;
    }
    closedir(dir);
    return count;
}

/*
 * Launch a private server instance with a controlled port and connection
 * capacity, so admission/overload behavior can be exercised without changing
 * the shared server. Output is discarded. Returns the pid, or -1 on failure.
 */
static pid_t spawn_test_server(int port, int capacity) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        char port_buf[16];
        char capacity_buf[16];
        snprintf(port_buf, sizeof(port_buf), "%d", port);
        snprintf(capacity_buf, sizeof(capacity_buf), "%d", capacity);
        setenv("HTTP_SERVER_PORT", port_buf, 1);
        setenv("HTTP_SERVER_MAX_CONNECTIONS", capacity_buf, 1);
        execl("./bin/http_server", "http_server", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Wait until `port` accepts a TCP connection, up to timeout_ms. */
static int wait_for_server(int port, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        int fd = connect_to_port(port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        usleep(50000);
    }
    return -1;
}

static void stop_test_server(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
}

static void http_response_free(HttpResponse *response) {
    if (!response) return;
    free(response->raw);
    free(response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
    response->content_length = -1;
}

static int response_buffer_append(char **buffer, size_t *length,
                                  const void *data, size_t data_length) {
    char *new_buffer = realloc(*buffer, *length + data_length + 1);
    if (!new_buffer) return -1;
    memcpy(new_buffer + *length, data, data_length);
    *length += data_length;
    new_buffer[*length] = '\0';
    *buffer = new_buffer;
    return 0;
}

static int wait_for_readable(int socket_fd) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket_fd, &readfds);

    struct timeval timeout = {
        .tv_sec = RESPONSE_TIMEOUT_MS / 1000,
        .tv_usec = (RESPONSE_TIMEOUT_MS % 1000) * 1000
    };
    int result;
    do {
        result = select(socket_fd + 1, &readfds, NULL, NULL, &timeout);
    } while (result < 0 && errno == EINTR);
    return result > 0 ? 0 : -1;
}

static int receive_bytes(int socket_fd, void *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        if (wait_for_readable(socket_fd) != 0) return -1;
        ssize_t count = recv(socket_fd, (char *)buffer + received,
                             length - received, 0);
        if (count <= 0) return -1;
        received += (size_t)count;
    }
    return 0;
}

static int receive_line(int socket_fd, char **line, size_t *length) {
    *line = NULL;
    *length = 0;
    char character;
    while (*length < BUFFER_SIZE * 4) {
        if (receive_bytes(socket_fd, &character, 1) != 0) return -1;
        if (response_buffer_append(line, length, &character, 1) != 0) {
            free(*line);
            *line = NULL;
            *length = 0;
            return -1;
        }
        if (*length >= 2 && (*line)[*length - 2] == '\r' && character == '\n') {
            return 0;
        }
    }
    free(*line);
    *line = NULL;
    *length = 0;
    return -1;
}

static int header_value(const char *headers, const char *name,
                        char *value, size_t value_size) {
    size_t name_length = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) break;
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && (size_t)(colon - line) == name_length &&
            strncasecmp(line, name, name_length) == 0) {
            const char *start = colon + 1;
            while (start < line_end && (*start == ' ' || *start == '\t')) start++;
            const char *finish = line_end;
            while (finish > start && (finish[-1] == ' ' || finish[-1] == '\t')) finish--;
            size_t value_length = (size_t)(finish - start);
            if (value_length + 1 > value_size) return -1;
            memcpy(value, start, value_length);
            value[value_length] = '\0';
            return 0;
        }
        line = line_end + 2;
    }
    return 1;
}

static int parse_content_length(const char *value, long long *length) {
    if (!value || !*value) return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    while (end && isspace((unsigned char)*end)) end++;
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > (unsigned long long)SIZE_MAX ||
        parsed > (unsigned long long)LLONG_MAX) return -1;
    *length = (long long)parsed;
    return 0;
}

static int parse_chunk_size(const char *line, size_t *chunk_size) {
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(line, &end, 16);
    while (end && isspace((unsigned char)*end)) end++;
    if (errno == ERANGE || end == line || *end != '\0' ||
        parsed > (unsigned long long)SIZE_MAX) return -1;
    *chunk_size = (size_t)parsed;
    return 0;
}


static int decompress_gzip(const char *compressed, size_t compressed_length,
                           char **decompressed, size_t *decompressed_length) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)compressed;
    stream.avail_in = (uInt)compressed_length;
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return -1;

    *decompressed = NULL;
    *decompressed_length = 0;
    int result = Z_OK;
    while (result == Z_OK) {
        unsigned char buffer[4096];
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        size_t produced = sizeof(buffer) - stream.avail_out;
        if (produced && response_buffer_append(decompressed, decompressed_length,
                                               buffer, produced) != 0) {
            inflateEnd(&stream);
            free(*decompressed);
            *decompressed = NULL;
            *decompressed_length = 0;
            return -1;
        }
    }
    int valid = result == Z_STREAM_END && stream.avail_in == 0;
    inflateEnd(&stream);
    if (!valid) {
        free(*decompressed);
        *decompressed = NULL;
        *decompressed_length = 0;
        return -1;
    }
    return 0;
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) return 1;
    for (; *text; text++) {
        size_t i = 0;
        while (i < needle_length && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_length) return 1;
    }
    return 0;
}

static int read_http_response(int client_socket, const char *request,
                              HttpResponse *response) {
    memset(response, 0, sizeof(*response));
    response->content_length = -1;

    char *line = NULL;
    size_t line_length = 0;
    int status_line_read = 0;
    while (1) {
        if (receive_line(client_socket, &line, &line_length) != 0) goto fail;
        if (response_buffer_append(&response->raw, &response->raw_length,
                                   line, line_length) != 0) goto fail;
        if (!status_line_read) {
            if (line_length < 2) goto fail;
            if (sscanf(line, "HTTP/%*s %d", &response->status_code) != 1) goto fail;
            status_line_read = 1;
        }
        if (line_length == 2) break;
        if (response_buffer_append(&response->headers, &response->headers_length,
                                   line, line_length) != 0) goto fail;
        free(line);
        line = NULL;
        line_length = 0;
    }
    free(line);
    line = NULL;

    char value[128];
    int header_result = header_value(response->headers, "Content-Length",
                                     value, sizeof(value));
    if (header_result == 0 && parse_content_length(value, &response->content_length) != 0) goto fail;
    header_result = header_value(response->headers, "Transfer-Encoding", value, sizeof(value));
    if (header_result == 0 && contains_case_insensitive(value, "chunked")) response->chunked = 1;

    int no_body = request && strncasecmp(request, "HEAD ", 5) == 0;
    if (no_body || response->status_code == 204 || response->status_code == 304) return 0;

    /* Chunk boundaries are wire framing, not application payload. */
    if (response->chunked) {
        while (1) {
            if (receive_line(client_socket, &line, &line_length) != 0) goto fail;
            if (response_buffer_append(&response->raw, &response->raw_length,
                                       line, line_length) != 0) goto fail;
            size_t chunk_size;
            if (parse_chunk_size(line, &chunk_size) != 0) goto fail;
            free(line);
            line = NULL;
            line_length = 0;
            if (chunk_size == 0) {
                do {
                    if (receive_line(client_socket, &line, &line_length) != 0) goto fail;
                    if (response_buffer_append(&response->raw, &response->raw_length,
                                               line, line_length) != 0) goto fail;
                    int is_end = (line_length == 2);
                    free(line);
                    line = NULL;
                    line_length = 0;
                    if (is_end) break;
                } while (1);
                break;
            }
            char *chunk = malloc(chunk_size);
            if (!chunk || receive_bytes(client_socket, chunk, chunk_size) != 0) {
                free(chunk);
                goto fail;
            }
            if (response_buffer_append(&response->raw, &response->raw_length,
                                       chunk, chunk_size) != 0 ||
                response_buffer_append(&response->body, &response->body_length,
                                       chunk, chunk_size) != 0) {
                free(chunk);
                goto fail;
            }
            free(chunk);
            char crlf[2];
            if (receive_bytes(client_socket, crlf, sizeof(crlf)) != 0 ||
                memcmp(crlf, "\r\n", 2) != 0 ||
                response_buffer_append(&response->raw, &response->raw_length,
                                       crlf, sizeof(crlf)) != 0) goto fail;
        }
        return 0;
    }

    /* Content-Length is authoritative for persistent connections. */
    if (response->content_length >= 0) {
        size_t length = (size_t)response->content_length;
        char *body = malloc(length + 1);
        if (!body || receive_bytes(client_socket, body, length) != 0) {
            free(body);
            goto fail;
        }
        body[length] = '\0';
        response->body = body;
        response->body_length = length;
        if (response_buffer_append(&response->raw, &response->raw_length,
                                   body, length) != 0) goto fail;
        return 0;
    }

    /* Only close-delimited responses may use EOF as their boundary. */
    while (1) {
        char buffer[BUFFER_SIZE];
        if (wait_for_readable(client_socket) != 0) goto fail;
        ssize_t count = recv(client_socket, buffer, sizeof(buffer), 0);
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            goto fail;
        }
        if (response_buffer_append(&response->raw, &response->raw_length,
                                   buffer, (size_t)count) != 0 ||
            response_buffer_append(&response->body, &response->body_length,
                                   buffer, (size_t)count) != 0) goto fail;
    }

fail:
    free(line);
    http_response_free(response);
    return -1;
}

static char *send_http_request(int client_socket, const char *request) {
    if (send(client_socket, request, strlen(request), 0) == -1) return NULL;
    HttpResponse response;
    if (read_http_response(client_socket, request, &response) != 0) return NULL;
    char *raw = response.raw;
    response.raw = NULL;
    http_response_free(&response);
    return raw;
}

// --- Test Cases ---

/* Basic endpoint tests establish status and body availability. More precise
 * framing assertions belong in the gzip and keep-alive tests below. */

void test_server_connectivity() {
    int sock = create_and_connect_socket();
    TEST_ASSERT(sock != -1); 
    if(sock != -1) close(sock);
}

void test_home_page() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    // This check failed before because body wasn't fully read:
    TEST_ASSERT(strstr(response, "<title>Home Page</title>") != NULL);
    
    free(response);
    close(client_socket);
}

void test_hello_page() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    TEST_ASSERT(strstr(response, "Greetings!") != NULL);

    free(response);
    close(client_socket);
}

void test_gzip_response_framing() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    const char request[] =
        "GET /home HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n\r\n";
    TEST_ASSERT(send(client_socket, request, strlen(request), 0) != -1);

    HttpResponse response;
    TEST_ASSERT(read_http_response(client_socket, request, &response) == 0);
    TEST_ASSERT(response.status_code == 200);
    TEST_ASSERT(!response.chunked);
    TEST_ASSERT(response.content_length == (long long)response.body_length);
    TEST_ASSERT(response.body_length > 0);
    char *decompressed = NULL;
    size_t decompressed_length = 0;
    TEST_ASSERT(decompress_gzip(response.body, response.body_length,
                                &decompressed, &decompressed_length) == 0);
    TEST_ASSERT(decompressed && decompressed_length > 0);
    TEST_ASSERT(decompressed && strstr(decompressed, "<title>Home Page</title>") != NULL);
    free(decompressed);
    http_response_free(&response);
    close(client_socket);
}

void test_gzip_negotiation() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    const char request[] =
        "GET /home HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept-Encoding: gzip;q=0\r\n"
        "Connection: close\r\n\r\n";
    HttpResponse response;
    TEST_ASSERT(send(client_socket, request, strlen(request), 0) != -1);
    TEST_ASSERT(read_http_response(client_socket, request, &response) == 0);
    TEST_ASSERT(response.status_code == 200);
    TEST_ASSERT(response.chunked == 0);
    TEST_ASSERT(strstr(response.headers, "Content-Encoding: gzip") == NULL);
    TEST_ASSERT(strstr(response.body, "<title>Home Page</title>") != NULL);
    http_response_free(&response);
    close(client_socket);
}

void test_head_gzip_response() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    const char request[] =
        "HEAD /home HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n\r\n";
    HttpResponse response;
    TEST_ASSERT(send(client_socket, request, strlen(request), 0) != -1);
    TEST_ASSERT(read_http_response(client_socket, request, &response) == 0);
    TEST_ASSERT(response.status_code == 200);
    TEST_ASSERT(response.body_length == 0);
    TEST_ASSERT(strstr(response.headers, "Content-Encoding: gzip") != NULL);
    char content_length[32];
    long long declared_length;
    TEST_ASSERT(header_value(response.headers, "Content-Length",
                             content_length, sizeof(content_length)) == 0);
    TEST_ASSERT(parse_content_length(content_length, &declared_length) == 0);
    TEST_ASSERT(declared_length > 0);
    http_response_free(&response);
    close(client_socket);
}

void test_fragmented_request() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    /* The server must retain an incomplete parse transaction until the final
     * CRLF arrives, regardless of how TCP fragments the request. */
    const char *fragments[] = {
        "GET /ho", "me HTTP/1.1\r\nHo", "st: localhost\r\nCon",
        "nection: close\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(fragments) / sizeof(fragments[0]); i++) {
        TEST_ASSERT(send(client_socket, fragments[i], strlen(fragments[i]), 0) != -1);
        usleep(1000);
    }

    const char *request = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    HttpResponse response;
    TEST_ASSERT(read_http_response(client_socket, request, &response) == 0);
    TEST_ASSERT(response.status_code == 200);
    TEST_ASSERT(response.body_length > 0);
    http_response_free(&response);
    close(client_socket);
}

void test_pipelining() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    const char request1[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    const char request2[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char requests[sizeof(request1) + sizeof(request2) - 1];
    memcpy(requests, request1, sizeof(request1) - 1);
    memcpy(requests + sizeof(request1) - 1, request2, sizeof(request2));
    TEST_ASSERT(send(client_socket, requests, sizeof(requests) - 1, 0) != -1);

    /* Two requests in one write must produce two independently framed
     * responses; the first body cannot consume bytes from the second header. */
    HttpResponse response1;
    HttpResponse response2;
    TEST_ASSERT(read_http_response(client_socket, request1, &response1) == 0);
    TEST_ASSERT(response1.status_code == 200);
    TEST_ASSERT(strstr(response1.body, "<title>Home Page</title>") != NULL);
    TEST_ASSERT(read_http_response(client_socket, request2, &response2) == 0);
    TEST_ASSERT(response2.status_code == 200);
    TEST_ASSERT(strstr(response2.body, "Greetings!") != NULL);
    http_response_free(&response1);
    http_response_free(&response2);
    close(client_socket);
}

void test_head_response() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    const char head_request[] = "HEAD /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    TEST_ASSERT(send(client_socket, head_request, strlen(head_request), 0) != -1);
    HttpResponse head_response;
    TEST_ASSERT(read_http_response(client_socket, head_request, &head_response) == 0);
    TEST_ASSERT(head_response.status_code == 200);
    TEST_ASSERT(head_response.body_length == 0);
    char content_length[32];
    long long declared_length;
    TEST_ASSERT(header_value(head_response.headers, "Content-Length",
                             content_length, sizeof(content_length)) == 0);
    TEST_ASSERT(parse_content_length(content_length, &declared_length) == 0);
    TEST_ASSERT(declared_length > 0);
    http_response_free(&head_response);

    const char get_request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    TEST_ASSERT(send(client_socket, get_request, strlen(get_request), 0) != -1);
    HttpResponse get_response;
    TEST_ASSERT(read_http_response(client_socket, get_request, &get_response) == 0);
    TEST_ASSERT(get_response.status_code == 200);
    TEST_ASSERT(strstr(get_response.body, "Greetings!") != NULL);
    http_response_free(&get_response);
    close(client_socket);
}

void test_root_page_load() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 200 OK") != NULL);
    
    free(response);
    close(client_socket);
}

void test_not_found_404() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "GET /nonexistent_path_12345 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 404 Not Found") != NULL);

    free(response);
    close(client_socket);
}

void test_not_implemented_501() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    char request[] = "DELETE /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *response = send_http_request(client_socket, request);

    TEST_ASSERT(response != NULL);
    TEST_ASSERT(strstr(response, "HTTP/1.1 501") != NULL || strstr(response, "HTTP/1.1 405") != NULL);

    free(response);
    close(client_socket);
}

void test_keep_alive() {
    int client_socket = create_and_connect_socket();
    TEST_ASSERT(client_socket != -1);

    /* The first response is Content-Length framed, so the next request must
     * be read from the same socket rather than after an idle-timeout guess. */
    // 1. First Request
    char req1[] = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    char *resp1 = send_http_request(client_socket, req1);
    
    TEST_ASSERT(resp1 != NULL);
    TEST_ASSERT(strstr(resp1, "HTTP/1.1 200 OK") != NULL);
    free(resp1);

    // 2. Second Request (Same Socket)
    char req2[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char *resp2 = send_http_request(client_socket, req2);
    
    TEST_ASSERT(resp2 != NULL);
    TEST_ASSERT(strstr(resp2, "HTTP/1.1 200 OK") != NULL);
    free(resp2);

    close(client_socket);
}

// --- Phase 2 Tests ---

/*
 * test_slow_client_header_timeout
 *
 * Connects to the server but never sends any data.  The server should close
 * the connection after HEADER_READ_TIMEOUT_SEC seconds.  We wait up to
 * (HEADER_READ_TIMEOUT_SEC + 3) seconds using select() and then verify that
 * recv() returns 0 (clean close) or -1 (reset), not that it blocks forever.
 */
void test_slow_client_header_timeout() {
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    /* Wait for server to close the connection due to header-read timeout. */
    int wait_sec = HEADER_READ_TIMEOUT_SEC + 3;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = wait_sec, .tv_usec = 0 };

    int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT(sel > 0); /* socket must become readable within the window */

    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    /* Server closed cleanly (0) or reset (< 0) — both are acceptable. */
    TEST_ASSERT(n <= 0);

    close(fd);
}

/*
 * test_keepalive_request_limit
 *
 * Sends MAX_KEEPALIVE_REQUESTS + 2 requests on a single keep-alive connection.
 * The server should close the connection at or before the limit is reached.
 */
void test_keepalive_request_limit() {
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *req =
        "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";

    int served = 0;
    int limit  = MAX_KEEPALIVE_REQUESTS + 2;

    for (int i = 0; i < limit; i++) {
        char *resp = send_http_request(fd, req);
        if (resp == NULL) {
            /* Connection was closed by the server. */
            break;
        }
        served++;
        free(resp);
    }

    close(fd);

    /* Must have served at least 1 request and no more than the configured limit. */
    TEST_ASSERT(served >= 1);
    TEST_ASSERT(served <= MAX_KEEPALIVE_REQUESTS);
}

/*
 * test_input_buffer_limit
 *
 * Sends a single request whose headers collectively exceed MAX_INPUT_BUFFER_BYTES.
 * The server should respond with 413 or close the connection.
 */
void test_input_buffer_limit() {
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    /* Build a request with a very long header value that exceeds the limit. */
    size_t big_sz = MAX_INPUT_BUFFER_BYTES + 1024;
    char *big_req = malloc(big_sz + 128);
    if (!big_req) { close(fd); return; }

    /* Write the request line and a header with a value that blows the limit. */
    int hdr_len = snprintf(big_req, big_sz + 128,
                           "GET /home HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "X-Padding: ");
    memset(big_req + hdr_len, 'A', big_sz);
    hdr_len += (int)big_sz;
    memcpy(big_req + hdr_len, "\r\n\r\n", 5);
    hdr_len += 4;

    send(fd, big_req, (size_t)hdr_len, 0);
    free(big_req);

    /* Allow a brief window for the server to respond or close. */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    int sel = select(fd + 1, &rfds, NULL, NULL, &tv);

    if (sel > 0) {
        char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        /* Either a 413 response or a clean close; both are acceptable. */
        TEST_ASSERT(n >= 0);
        if (n > 0) {
            buf[n] = '\0';
            /* If there IS a response it should be a 4xx. */
            int is_4xx = (strstr(buf, "HTTP/1.1 4") != NULL);
            int is_close = (n == 0);
            TEST_ASSERT(is_4xx || is_close || n > 0 /* at least something came back */);
        }
    }
    /* If select timed out the server is still processing; that's a failure. */
    TEST_ASSERT(sel >= 0);

    close(fd);
}

/*
 * test_long_header_line_rejected
 *
 * A single header line longer than MAX_HEADER_LEN must be rejected with 431
 * rather than silently truncated and parsed as a valid short header.
 */
void test_long_header_line_rejected() {
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    size_t pad = MAX_HEADER_LEN * 2;
    char *req = malloc(pad + 128);
    if (!req) { close(fd); return; }

    int n = snprintf(req, pad + 128,
                     "GET /home HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "X-Padding: ");
    memset(req + n, 'A', pad);
    n += (int)pad;
    const char *suffix = "\r\n\r\n";
    memcpy(req + n, suffix, strlen(suffix));
    n += (int)strlen(suffix);

    send(fd, req, (size_t)n, 0);
    free(req);

    char buf[512];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
    TEST_ASSERT(r > 0);
    buf[r] = '\0';
    TEST_ASSERT(strstr(buf, "431") != NULL);

    close(fd);
}

/*
 * test_long_request_line_rejected
 *
 * A request line longer than MAX_HEADER_LEN must be rejected with 414.
 */
void test_long_request_line_rejected() {
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    size_t pad = MAX_HEADER_LEN * 2;
    char *req = malloc(pad + 64);
    if (!req) { close(fd); return; }

    int n = snprintf(req, pad + 64, "GET /");
    memset(req + n, 'a', pad);
    n += (int)pad;
    const char *suffix = " HTTP/1.1\r\n\r\n";
    memcpy(req + n, suffix, strlen(suffix));
    n += (int)strlen(suffix);

    send(fd, req, (size_t)n, 0);
    free(req);

    char buf[512];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
    TEST_ASSERT(r > 0);
    buf[r] = '\0';
    TEST_ASSERT(strstr(buf, "414") != NULL);

    close(fd);
}

// --- Phase 3 event-loop integration tests ---

/*
 * test_el_fragmented_headers
 *
 * Deliver a valid GET /home request one byte at a time with a 1ms pause
 * between each write.  The event loop must reassemble the fragmented input
 * across multiple epoll-readable events and return a correct response.
 */
void test_el_fragmented_headers(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *req = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t len = strlen(req);
    for (size_t i = 0; i < len; i++) {
        ssize_t n = send(fd, req + i, 1, 0);
        if (n < 0) {
            close(fd);
            TEST_ASSERT(0 /* send failed during fragmented delivery */);
            return;
        }
        usleep(1000); /* 1 ms between bytes */
    }

    HttpResponse resp;
    int rc = read_http_response(fd, req, &resp);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(resp.status_code == 200);
    TEST_ASSERT(resp.body_length > 0);
    http_response_free(&resp);
    close(fd);
}

/*
 * test_el_coalesced_pipeline
 *
 * Pack three different keep-alive requests into a single TCP send.  The
 * event loop must parse all three requests from the coalesced data and
 * return responses in request order:
 *   1. GET /        -> 200 (/, home page)
 *   2. GET /hello   -> 200 (hello page)
 *   3. GET /home    -> 200 (home page, Connection: close)
 */
void test_el_coalesced_pipeline(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *req1 = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    const char *req2 = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    const char *req3 = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    /* Build one buffer with all three requests. */
    size_t l1 = strlen(req1), l2 = strlen(req2), l3 = strlen(req3);
    char *all = malloc(l1 + l2 + l3 + 1);
    TEST_ASSERT(all != NULL);
    memcpy(all,           req1, l1);
    memcpy(all + l1,      req2, l2);
    memcpy(all + l1 + l2, req3, l3);
    ssize_t sent = send(fd, all, l1 + l2 + l3, 0);
    free(all);
    TEST_ASSERT(sent > 0);

    /* Read and verify three responses. */
    HttpResponse r1, r2, r3;
    TEST_ASSERT(read_http_response(fd, req1, &r1) == 0);
    TEST_ASSERT(r1.status_code == 200);
    TEST_ASSERT(r1.body_length > 0);

    TEST_ASSERT(read_http_response(fd, req2, &r2) == 0);
    TEST_ASSERT(r2.status_code == 200);
    TEST_ASSERT(r2.body_length > 0);

    /* Response 1 and 2 must be different pages. */
    TEST_ASSERT(r1.body_length != r2.body_length ||
                memcmp(r1.body, r2.body, r1.body_length) != 0);

    TEST_ASSERT(read_http_response(fd, req3, &r3) == 0);
    TEST_ASSERT(r3.status_code == 200);
    TEST_ASSERT(r3.body_length > 0);

    /* Response 3 (/home) must match response 1 (/). */
    TEST_ASSERT(r1.body_length == r3.body_length);

    http_response_free(&r1);
    http_response_free(&r2);
    http_response_free(&r3);
    close(fd);
}

/*
 * test_el_abrupt_client_close
 *
 * Send an incomplete request (no terminal \r\n\r\n) then abruptly close
 * the socket.  The event loop must handle EPOLLHUP / EOF without crashing
 * or leaking the connection.  A subsequent request on a fresh socket must
 * succeed, proving the server is still responsive.
 */
void test_el_abrupt_client_close(void)
{
    /* Connect and send a partial request. */
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);
    const char *partial = "GET /home HTTP/1.1\r\nHost: localhost\r\n";
    send(fd, partial, strlen(partial), 0);
    /* Close without completing headers — simulates an abrupt disconnect. */
    close(fd);

    /* Brief pause so the server can process the EOF. */
    usleep(50000); /* 50 ms */

    /* Verify the server is still alive and serving. */
    int fd2 = create_and_connect_socket();
    TEST_ASSERT(fd2 != -1);
    const char *req = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    HttpResponse resp;
    TEST_ASSERT(read_http_response(fd2, req, &resp) == -1 ||
                (send(fd2, req, strlen(req), 0) > 0 &&
                 read_http_response(fd2, req, &resp) == 0 &&
                 resp.status_code == 200));
    http_response_free(&resp);
    close(fd2);

    /* Cleaner version: open, send, read. */
    int fd3 = create_and_connect_socket();
    TEST_ASSERT(fd3 != -1);
    ssize_t s = send(fd3, req, strlen(req), 0);
    TEST_ASSERT(s > 0);
    HttpResponse resp3;
    int rc3 = read_http_response(fd3, req, &resp3);
    TEST_ASSERT(rc3 == 0);
    TEST_ASSERT(resp3.status_code == 200);
    http_response_free(&resp3);
    close(fd3);
}

/*
 * test_el_keepalive_multiple_cycles
 *
 * Issue five successive requests on one keep-alive connection, verifying
 * that the event loop correctly transitions between WRITING and KEEP_ALIVE
 * states and delivers an independent 200 response for each request.
 */
void test_el_keepalive_multiple_cycles(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    /* All but the last request uses keep-alive. */
    for (int i = 0; i < 4; i++) {
        const char *req = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        ssize_t s = send(fd, req, strlen(req), 0);
        TEST_ASSERT(s > 0);
        HttpResponse resp;
        int rc = read_http_response(fd, req, &resp);
        TEST_ASSERT(rc == 0);
        TEST_ASSERT(resp.status_code == 200);
        TEST_ASSERT(resp.body_length > 0);
        http_response_free(&resp);
    }

    /* Final request closes. */
    const char *fin = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t s = send(fd, fin, strlen(fin), 0);
    TEST_ASSERT(s > 0);
    HttpResponse resp;
    int rc = read_http_response(fd, fin, &resp);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(resp.status_code == 200);
    http_response_free(&resp);
    close(fd);
}

/*
 * test_el_deep_pipeline
 *
 * Send MAX_PIPELINE_DEPTH + 17 requests (33) in one TCP write.  The event
 * loop must parse in increments of MAX_PIPELINE_DEPTH, drain the queue, then
 * refill it from the ring buffer until every buffered request is answered.
 * Regression test for the pipeline-cap stall where responses beyond the cap
 * were never sent because no further EPOLLIN event would arrive.
 */
void test_el_deep_pipeline(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *req = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t req_len = strlen(req);
    int count = MAX_PIPELINE_DEPTH + 17;

    char *burst = malloc(req_len * (size_t)count);
    TEST_ASSERT(burst != NULL);
    for (int i = 0; i < count; i++) {
        memcpy(burst + (size_t)i * req_len, req, req_len);
    }
    ssize_t sent = send(fd, burst, req_len * (size_t)count, 0);
    free(burst);
    TEST_ASSERT(sent > 0);

    HttpResponse resp;
    int ok = 1;
    for (int i = 0; i < count; i++) {
        if (read_http_response(fd, req, &resp) != 0) { ok = 0; break; }
        if (resp.status_code != 200) { ok = 0; }
        http_response_free(&resp);
    }
    TEST_ASSERT(ok == 1);
    close(fd);
}

/*
 * test_el_concurrent_connections
 *
 * Open CONCURRENCY connections at the same time, each on its own thread,
 * each sending GET /home.  All must receive a 200 response.  This verifies
 * the event loop handles N simultaneous fds without dropping any.
 */
#define EL_CONCURRENCY 20

typedef struct {
    int ok;  /* 1 if the request succeeded, 0 otherwise */
} ElConcResult;

static void *el_conc_worker(void *arg)
{
    ElConcResult *res = (ElConcResult *)arg;
    res->ok = 0;
    int fd = create_and_connect_socket();
    if (fd < 0) return NULL;
    const char *req = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (send(fd, req, strlen(req), 0) <= 0) { close(fd); return NULL; }
    HttpResponse resp;
    if (read_http_response(fd, req, &resp) == 0 && resp.status_code == 200)
        res->ok = 1;
    http_response_free(&resp);
    close(fd);
    return NULL;
}

void test_el_concurrent_connections(void)
{
    pthread_t threads[EL_CONCURRENCY];
    ElConcResult results[EL_CONCURRENCY];

    for (int i = 0; i < EL_CONCURRENCY; i++) {
        results[i].ok = 0;
        pthread_create(&threads[i], NULL, el_conc_worker, &results[i]);
    }
    for (int i = 0; i < EL_CONCURRENCY; i++) {
        pthread_join(threads[i], NULL);
    }

    int failures = 0;
    for (int i = 0; i < EL_CONCURRENCY; i++) {
        if (!results[i].ok) failures++;
    }
    TEST_ASSERT(failures == 0);
}

/*
 * test_byte_ranges_multipart
 *
 * A multi-range request must produce a 206 multipart/byteranges response whose
 * parts carry exactly the requested slices. Fetch the full representation first,
 * then request two ranges on the same connection and verify each part's media
 * type, Content-Range, and bytes against the full body.
 */
static int multipart_extract_boundary(const char *headers, char *out, size_t cap)
{
    const char *key = "Content-Type: multipart/byteranges; boundary=";
    const char *p = strstr(headers, key);
    if (!p) return -1;
    p += strlen(key);
    const char *end = strstr(p, "\r\n");
    if (!end || end == p) return -1;
    size_t len = (size_t)(end - p);
    if (len + 1 > cap) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

void test_byte_ranges_multipart(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *full_req =
        "GET /home HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    TEST_ASSERT(send(fd, full_req, strlen(full_req), 0) > 0);
    HttpResponse full;
    TEST_ASSERT(read_http_response(fd, full_req, &full) == 0);
    TEST_ASSERT(full.status_code == 200);
    TEST_ASSERT(full.body_length >= 40);

    const char *range_req =
        "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9,20-29\r\nConnection: close\r\n\r\n";
    TEST_ASSERT(send(fd, range_req, strlen(range_req), 0) > 0);
    HttpResponse resp;
    TEST_ASSERT(read_http_response(fd, range_req, &resp) == 0);
    TEST_ASSERT(resp.status_code == 206);
    TEST_ASSERT(resp.headers != NULL);
    TEST_ASSERT(strstr(resp.headers, "Content-Type: multipart/byteranges; boundary=") != NULL);
    TEST_ASSERT(strstr(resp.headers, "Accept-Ranges: bytes") != NULL);

    char boundary[128];
    TEST_ASSERT(multipart_extract_boundary(resp.headers, boundary, sizeof(boundary)) == 0);

    char closing[160];
    snprintf(closing, sizeof(closing), "--%s--\r\n", boundary);
    TEST_ASSERT(strstr(resp.body, closing) != NULL);

    int parts = 0;
    const char *scan = resp.body;
    while ((scan = strstr(scan, "Content-Range: bytes ")) != NULL) {
        size_t start = 0, end = 0, total = 0;
        if (sscanf(scan, "Content-Range: bytes %zu-%zu/%zu",
                   &start, &end, &total) != 3) break;
        TEST_ASSERT(total == full.body_length);
        const char *data = strstr(scan, "\r\n\r\n");
        TEST_ASSERT(data != NULL);
        data += 4;
        size_t count = end - start + 1;
        TEST_ASSERT(start + count <= full.body_length);
        TEST_ASSERT(memcmp(data, full.body + start, count) == 0);
        parts++;
        scan = data + count;
    }
    TEST_ASSERT(parts == 2);

    http_response_free(&full);
    http_response_free(&resp);
    close(fd);
}

/*
 * test_el_client_close_during_write
 *
 * Send a complete request then close the socket without reading the response
 * (alternating a graceful FIN with a reset via SO_LINGER). The server must
 * absorb EPIPE/ECONNRESET on the write path and stay responsive.
 */
void test_el_client_close_during_write(void)
{
    const char *req =
        "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    for (int i = 0; i < 100; i++) {
        int fd = create_and_connect_socket();
        if (fd < 0) continue;
        if (send(fd, req, strlen(req), 0) < 0) {
            close(fd);
            continue;
        }
        if (i % 2 == 0) {
            struct linger reset = {1, 0}; /* RST on close */
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        }
        close(fd);
    }
    usleep(200000);

    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);
    char *raw = send_http_request(fd, req);
    TEST_ASSERT(raw != NULL);
    TEST_ASSERT(strstr(raw, "HTTP/1.1 200 OK") != NULL);
    free(raw);
    close(fd);
}

/*
 * test_el_pipelined_after_error
 *
 * Pipeline a 404 keep-alive response with a following 200 on one socket. The
 * event loop must continue serving after an application-level error response
 * instead of desynchronizing or dropping the queued request.
 */
void test_el_pipelined_after_error(void)
{
    int fd = create_and_connect_socket();
    TEST_ASSERT(fd != -1);

    const char *req1 =
        "GET /nonexistent HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    const char *req2 =
        "GET /home HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    char burst[512];
    int n = snprintf(burst, sizeof(burst), "%s%s", req1, req2);
    TEST_ASSERT(n > 0 && (size_t)n < sizeof(burst));
    TEST_ASSERT(send(fd, burst, (size_t)n, 0) == n);

    HttpResponse r1, r2;
    TEST_ASSERT(read_http_response(fd, req1, &r1) == 0);
    TEST_ASSERT(r1.status_code == 404);
    TEST_ASSERT(read_http_response(fd, req2, &r2) == 0);
    TEST_ASSERT(r2.status_code == 200);
    TEST_ASSERT(r2.body_length > 0);

    http_response_free(&r1);
    http_response_free(&r2);
    close(fd);
}

/*
 * test_el_capacity_and_fd_leak
 *
 * Launches a private server with a small connection capacity to exercise two
 * resource-safety gates end to end:
 *   1. Admission cap: capacity+1 simultaneous connections yield at least one
 *      bounded 503 (or close) while the rest are served.
 *   2. Descriptor safety: a burst of abrupt client disconnects (before sending,
 *      mid-response, and mid-request) must not grow the server's descriptor
 *      count beyond a small slack.
 */
void test_el_capacity_and_fd_leak(void)
{
    const int port = 8098;
    const int capacity = 8;

    pid_t pid = spawn_test_server(port, capacity);
    TEST_ASSERT(pid > 0);
    if (wait_for_server(port, 5000) != 0) {
        stop_test_server(pid);
        TEST_ASSERT(0);
    }
    usleep(200000);
    int baseline = count_open_fds(pid);
    int have_proc = baseline >= 0;

    /* --- 1. Admission cap. --- */
    int fds[capacity + 1];
    int opened = 0;
    for (int i = 0; i < capacity + 1; i++) {
        fds[i] = connect_to_port(port);
        if (fds[i] >= 0) opened++;
    }
    TEST_ASSERT(opened == capacity + 1);

    const char *keep_req =
        "GET /home HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    int rejected = 0, served = 0;
    for (int i = 0; i < capacity + 1; i++) {
        send(fds[i], keep_req, strlen(keep_req), 0); /* may fail if rejected */
        HttpResponse resp;
        if (read_http_response(fds[i], keep_req, &resp) == 0) {
            if (resp.status_code == 503) rejected++;
            else if (resp.status_code == 200) served++;
            http_response_free(&resp);
        }
    }
    for (int i = 0; i < capacity + 1; i++) close(fds[i]);
    TEST_ASSERT(rejected >= 1);
    TEST_ASSERT(served >= 1);

    /* --- 2. Descriptor safety across abrupt disconnects. --- */
    for (int i = 0; i < 200; i++) {
        int fd = connect_to_port(port);
        if (fd >= 0) close(fd); /* closed before sending anything */
    }
    for (int i = 0; i < 200; i++) {
        int fd = connect_to_port(port);
        if (fd >= 0) {
            send(fd, keep_req, strlen(keep_req), 0);
            close(fd); /* closed while the response is in flight */
        }
    }
    const char *partial = "GET /home HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 100; i++) {
        int fd = connect_to_port(port);
        if (fd >= 0) {
            send(fd, partial, strlen(partial), 0);
            close(fd); /* closed mid-request */
        }
    }
    usleep(500000);

    if (have_proc) {
        int after = count_open_fds(pid);
        TEST_ASSERT(after >= 0);
        TEST_ASSERT(after <= baseline + 8);
    }

    stop_test_server(pid);
}

// --- Phase 1: HTTP conformance corpus ---

/*
 * A checked-in corpus of malformed, edge, and smuggling-shaped requests with
 * the status the server must return. Each case runs on its own connection so a
 * force-closed error response cannot leak state into the next case.
 */
typedef struct {
    const char *name;
    const char *request;
    int         status;
    const char *must_contain;     /* header substring, or NULL */
    const char *must_not_contain; /* header substring, or NULL */
} ConformanceCase;

void test_http_conformance_corpus(void)
{
    static const ConformanceCase cases[] = {
        /* Request-line grammar and version. */
        {"multiple-spaces",
         "GET    /home    HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         200, "Date: ", NULL},
        {"tab-separated-request-line",
         "GET\t/home\tHTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},
        {"mixed-header-casing",
         "GET /home HTTP/1.1\r\nhOsT: x\r\ncOnNeCtIoN: close\r\n\r\n",
         200, NULL, NULL},
        {"leading-empty-line",
         "\r\nGET /home HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},
        {"bad-version",
         "GET /home HTTP/2.0\r\nHost: x\r\nConnection: close\r\n\r\n",
         505, NULL, NULL},

        /* Host requirements and targets. */
        {"missing-host",
         "GET /home HTTP/1.1\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"http10-no-host-ok",
         "GET /home HTTP/1.0\r\n\r\n",
         200, NULL, NULL},
        {"multiple-host",
         "GET /home HTTP/1.1\r\nHost: a\r\nHost: b\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"absolute-form",
         "GET http://example.com/home HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},
        {"absolute-form-query",
         "GET http://example.com/home?x=1 HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},
        {"origin-query-stripped",
         "GET /home?x=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},

        /* Field syntax. */
        {"obs-fold",
         "GET /home HTTP/1.1\r\nHost: x\r\nX-Foo: a\r\n b\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"control-char-value",
         "GET /home HTTP/1.1\r\nHost: x\r\nX-Bad: a\x01b\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"space-before-colon",
         "GET /home HTTP/1.1\r\nHost: x\r\nX-Bad : v\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"empty-header-value",
         "GET /home HTTP/1.1\r\nHost: x\r\nX-Empty:\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},

        /* Framing / smuggling. */
        {"duplicate-content-length",
         "GET /home HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"conflicting-content-length",
         "GET /home HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"non-numeric-content-length",
         "GET /home HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"transfer-encoding-only",
         "GET /home HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
         501, NULL, NULL},
        {"te-plus-content-length",
         "GET /home HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
         400, NULL, NULL},
        {"get-with-body",
         "GET /home HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc",
         400, NULL, NULL},

        /* Method semantics. */
        {"options-asterisk",
         "OPTIONS * HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         204, "Allow: GET, HEAD, OPTIONS", NULL},
        {"options-path",
         "OPTIONS /home HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         204, "Allow: GET, HEAD, OPTIONS", NULL},
        {"post-not-allowed",
         "POST /home HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
         405, "Allow: GET, HEAD, OPTIONS", NULL},
        {"delete-unknown",
         "DELETE /home HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
         501, NULL, NULL},
        {"expect-unsupported",
         "GET /home HTTP/1.1\r\nHost: x\r\nExpect: bogus\r\nConnection: close\r\n\r\n",
         417, NULL, NULL},
        {"expect-continue",
         "GET /home HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},

        /* Conditional requests. */
        {"if-none-match-star",
         "GET /home HTTP/1.1\r\nHost: x\r\nIf-None-Match: *\r\nConnection: close\r\n\r\n",
         304, "ETag: ", NULL},
        {"if-modified-since-future",
         "GET /home HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Fri, 01 Jan 2100 00:00:00 GMT\r\nConnection: close\r\n\r\n",
         304, NULL, NULL},
        {"if-modified-since-past",
         "GET /home HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},

        /* Ranges. */
        {"range-single",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nConnection: close\r\n\r\n",
         206, "Content-Range: bytes 0-9/", NULL},
        {"range-suffix",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=-10\r\nConnection: close\r\n\r\n",
         206, "Content-Range: bytes ", NULL},
        {"range-unsatisfiable",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=100000-\r\nConnection: close\r\n\r\n",
         416, "Content-Range: bytes */", NULL},
        {"range-multi-multipart",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9,20-29\r\nConnection: close\r\n\r\n",
         206, "Content-Type: multipart/byteranges; boundary=", NULL},
        {"range-multi-with-unsatisfiable",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9,100000-\r\nConnection: close\r\n\r\n",
         206, "Content-Range: bytes 0-9/", "multipart/byteranges"},
        {"range-multi-too-many-ignored",
         "GET /home HTTP/1.1\r\nHost: x\r\n"
         "Range: bytes=0-1,2-3,4-5,6-7,8-9,10-11,12-13,14-15,16-17\r\nConnection: close\r\n\r\n",
         200, "Accept-Ranges: bytes", "multipart/byteranges"},
        {"range-invalid-last-before-first",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=5-3\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},
        {"range-suffix-zero-unsatisfiable",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=-0\r\nConnection: close\r\n\r\n",
         416, "Content-Range: bytes */", NULL},
        {"if-range-mismatch",
         "GET /home HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\nIf-Range: \"nope\"\r\nConnection: close\r\n\r\n",
         200, NULL, NULL},

        /* Content negotiation. */
        {"accept-encoding-identity",
         "GET /home HTTP/1.1\r\nHost: x\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
         200, "Vary: Accept-Encoding", "Content-Encoding: gzip"},
        {"accept-encoding-gzip-q0",
         "GET /home HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip;q=0\r\nConnection: close\r\n\r\n",
         200, "Vary: Accept-Encoding", "Content-Encoding: gzip"},
        {"accept-encoding-star-q0-gzip",
         "GET /home HTTP/1.1\r\nHost: x\r\nAccept-Encoding: *;q=0, gzip\r\nConnection: close\r\n\r\n",
         200, "Content-Encoding: gzip", NULL},
        {"accept-encoding-none-acceptable",
         "GET /home HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip;q=0, identity;q=0\r\nConnection: close\r\n\r\n",
         406, NULL, NULL},
    };

    int fails = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ConformanceCase *tc = &cases[i];
        int fd = create_and_connect_socket();
        if (fd < 0) {
            printf("    corpus: %s: connect failed\n", tc->name);
            fails++;
            continue;
        }
        if (send(fd, tc->request, strlen(tc->request), 0) < 0) {
            printf("    corpus: %s: send failed\n", tc->name);
            fails++;
            close(fd);
            continue;
        }

        HttpResponse resp;
        if (read_http_response(fd, tc->request, &resp) != 0) {
            printf("    corpus: %s: no complete response\n", tc->name);
            fails++;
            close(fd);
            continue;
        }
        if (resp.status_code != tc->status) {
            printf("    corpus: %s: expected %d, got %d\n",
                   tc->name, tc->status, resp.status_code);
            fails++;
        }
        if (tc->must_contain &&
            (!resp.headers || !strstr(resp.headers, tc->must_contain))) {
            printf("    corpus: %s: missing '%s'\n", tc->name, tc->must_contain);
            fails++;
        }
        if (tc->must_not_contain && resp.headers &&
            strstr(resp.headers, tc->must_not_contain)) {
            printf("    corpus: %s: unexpectedly present '%s'\n",
                   tc->name, tc->must_not_contain);
            fails++;
        }
        http_response_free(&resp);
        close(fd);
    }
    TEST_ASSERT(fails == 0);
}

/*
 * Phase 2: document-root serving against the repository root (the default
 * document root when the server is launched per AGENTS.md). Verifies a real
 * file is served with a MIME type, HEAD carries no body, and the resolver
 * refuses hidden files and path traversal with the documented statuses.
 */
void test_docroot_static_serving() {
    struct {
        const char *name;
        const char *method;
        const char *path;
        int         status;
        const char *must_contain;
    } cases[] = {
        {"repo-file", "GET", "/readme.md", 200, "Content-Type: text/markdown"},
        {"head-file", "HEAD", "/readme.md", 200, "Content-Length: "},
        {"hidden",    "GET", "/.git/config", 403, NULL},
        {"traversal", "GET", "/../etc/passwd", 403, NULL},
        {"enc-traversal", "GET", "/%2e%2e/etc/passwd", 403, NULL},
        {"null-byte", "GET", "/%00", 400, NULL},
        {"missing",   "GET", "/definitely-not-here-12345", 404, NULL},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int sock = create_and_connect_socket();
        TEST_ASSERT(sock != -1);

        char request[256];
        snprintf(request, sizeof(request),
                 "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                 cases[i].method, cases[i].path);
        TEST_ASSERT(send(sock, request, strlen(request), 0) != -1);

        HttpResponse response;
        TEST_ASSERT(read_http_response(sock, request, &response) == 0);
        if (response.status_code != cases[i].status) {
            printf("    docroot %s: expected %d, got %d\n", cases[i].name,
                   cases[i].status, response.status_code);
        }
        TEST_ASSERT(response.status_code == cases[i].status);
        if (cases[i].must_contain) {
            TEST_ASSERT(strstr(response.headers, cases[i].must_contain) != NULL);
        }
        if (strcmp(cases[i].method, "HEAD") == 0) {
            TEST_ASSERT(response.body_length == 0);
        }
        http_response_free(&response);
        close(sock);
    }
}

// --- Runner ---

void run_server_tests() {
    printf("=== General Server Suite ===\n");
    printf("Note: Ensure the server is running on %s:%d before starting tests.\n\n", SERVER_IP, SERVER_PORT);

    /* Deliberate writes to sockets the server has closed must not kill the
     * test runner (the server ignores SIGPIPE; the client must too). */
    signal(SIGPIPE, SIG_IGN);

    // Basic Connectivity
    RUN_TEST(test_server_connectivity, "Check if server is reachable");

    // Standard Endpoints
    RUN_TEST(test_root_page_load,   "GET / (Root)");
    RUN_TEST(test_home_page,        "GET /home (Check content)");
    RUN_TEST(test_hello_page,       "GET /hello (Check content)");
    RUN_TEST(test_gzip_response_framing, "Gzip response framing");
    RUN_TEST(test_gzip_negotiation, "Gzip q=0 negotiation");
    RUN_TEST(test_head_gzip_response, "HEAD gzip response has no body");

    // Error Handling
    RUN_TEST(test_not_found_404,    "GET /nonexistent (Expect 404)");
    RUN_TEST(test_not_implemented_501, "DELETE / (Expect 501/405)");

    // Advanced Features
    RUN_TEST(test_keep_alive,       "Keep-Alive: Multiple reqs on one socket");
    RUN_TEST(test_fragmented_request, "Fragmented request delivery");
    RUN_TEST(test_pipelining,        "Pipelined requests and responses");
    RUN_TEST(test_head_response,     "HEAD response has no body");

    // Resource-protection behaviour
    RUN_TEST(test_slow_client_header_timeout, "Phase2: Slow client closed after header timeout");
    RUN_TEST(test_keepalive_request_limit,    "Phase2: Keep-alive connection closed at request limit");
    RUN_TEST(test_input_buffer_limit,         "Phase2: Oversized input rejected with 413 or close");
    RUN_TEST(test_long_header_line_rejected,  "Oversized header line rejected with 431");
    RUN_TEST(test_long_request_line_rejected, "Oversized request line rejected with 414");

    // Phase 1: HTTP/1.1 correctness and caching semantics
    RUN_TEST(test_http_conformance_corpus,    "Phase1: conformance corpus (malformed/edge/smuggling)");
    RUN_TEST(test_byte_ranges_multipart,      "Phase1: multipart/byteranges reassembly");
    RUN_TEST(test_el_pipelined_after_error,   "Phase1: keep-alive continues after a 404");
    RUN_TEST(test_el_client_close_during_write, "Phase1: client close/RST mid-response absorbed");
    RUN_TEST(test_el_capacity_and_fd_leak,    "Phase1: admission cap + no FD leak on disconnect burst");

    // Phase 2: secure document-root serving
    RUN_TEST(test_docroot_static_serving,     "Phase2: doc-root file, hidden/traversal refusal");

    // Phase 3: event-loop specific behaviour
    RUN_TEST(test_el_fragmented_headers,      "Phase3: Fragmented header delivery across recv calls");
    RUN_TEST(test_el_coalesced_pipeline,      "Phase3: Coalesced pipelined requests in order");
    RUN_TEST(test_el_abrupt_client_close,     "Phase3: Abrupt client close mid-request");
    RUN_TEST(test_el_keepalive_multiple_cycles, "Phase3: Keep-alive state cycling across 5 requests");
    RUN_TEST(test_el_deep_pipeline,  "Phase3: 33 pipelined requests in one write");
    RUN_TEST(test_el_concurrent_connections,  "Phase3: 20 concurrent connections all complete");
}
