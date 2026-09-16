#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/select.h> // Required for select()
#include <sys/time.h>   // Required for struct timeval
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <zlib.h>
#include "test_utils.h"
#include "server_test.h"

// --- Configuration ---
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8081
#define BUFFER_SIZE 4096
#define RESPONSE_TIMEOUT_MS 5000

// --- Helper Structures ---
typedef struct {
    int thread_id;
    int client_socket;
    const char* request;
    char* response;
} ThreadArgs;

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

/* Validate the encoded chunk stream independently of the response parser.
 * This catches invalid sizes, missing CRLF delimiters, and missing trailers. */
static int validate_chunked_wire(const HttpResponse *response) {
    const char *cursor = strstr(response->raw, "\r\n\r\n");
    if (!cursor) return -1;
    cursor += 4;
    const char *end = response->raw + response->raw_length;
    size_t decoded_length = 0;

    while (cursor < end) {
        const char *line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end >= end) return -1;
        size_t line_length = (size_t)(line_end - cursor);
        char *size_line = malloc(line_length + 1);
        if (!size_line) return -1;
        memcpy(size_line, cursor, line_length);
        size_line[line_length] = '\0';

        size_t chunk_size;
        int parsed = parse_chunk_size(size_line, &chunk_size);
        free(size_line);
        if (parsed != 0) return -1;
        cursor = line_end + 2;

        if (chunk_size == 0) {
            /* A zero chunk is followed by zero or more trailers and a final
             * empty line, not by another payload chunk. */
            while (cursor < end) {
                line_end = strstr(cursor, "\r\n");
                if (!line_end || line_end >= end) return -1;
                if (line_end == cursor) {
                    return cursor + 2 == end && decoded_length == response->body_length ? 0 : -1;
                }
                cursor = line_end + 2;
            }
            return -1;
        }

        if ((size_t)(end - cursor) < chunk_size + 2) return -1;
        decoded_length += chunk_size;
        cursor += chunk_size;
        if (memcmp(cursor, "\r\n", 2) != 0) return -1;
        cursor += 2;
    }
    return -1;
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

// Thread worker function
static void* multithread_worker(void* thread_arg) {
    ThreadArgs* args = (ThreadArgs*)thread_arg;
    args->response = send_http_request(args->client_socket, args->request);
    close(args->client_socket);
    return NULL;
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
    TEST_ASSERT(response.chunked);
    TEST_ASSERT(validate_chunked_wire(&response) == 0);
    /* A valid chunked response must expose payload bytes separately from its
     * chunk-size lines and terminating zero chunk. */
    TEST_ASSERT(response.body_length > 0);
    TEST_ASSERT(response.raw_length > response.body_length);
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

// --- Multithreaded Tests (Wrappers) ---

void test_multithread_load() {
    int num_threads = 17;
    pthread_t threads[num_threads];
    ThreadArgs args[num_threads];

    printf("       -> Spawning %d threads for concurrent requests...\n", num_threads);

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].client_socket = create_and_connect_socket();
        
        TEST_ASSERT(args[i].client_socket != -1);
        
        args[i].request = "GET /home HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        args[i].response = NULL;

        if (pthread_create(&threads[i], NULL, multithread_worker, &args[i]) != 0) {
            close(args[i].client_socket);
            TEST_ASSERT(0 && "Thread creation failed");
        }
    }

    // Join and Verify
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        
        TEST_ASSERT(args[i].response != NULL);
        if (args[i].response) {
            TEST_ASSERT(strstr(args[i].response, "HTTP/1.1 200 OK") != NULL);
            free(args[i].response);
        }
    }
}

void test_gzip_concurrency() {
    int num_threads = 5;
    pthread_t threads[num_threads];
    ThreadArgs args[num_threads];

    printf("       -> Spawning %d threads requesting GZIP...\n", num_threads);

    const char *gzip_req = "GET /home HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].client_socket = create_and_connect_socket();
        TEST_ASSERT(args[i].client_socket != -1);
        
        args[i].request = gzip_req;
        args[i].response = NULL;

        if (pthread_create(&threads[i], NULL, multithread_worker, &args[i]) != 0) {
            close(args[i].client_socket);
            TEST_ASSERT(0 && "Thread creation failed");
        }
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        
        TEST_ASSERT(args[i].response != NULL);
        if (args[i].response) {
            TEST_ASSERT(strstr(args[i].response, "HTTP/1.1 200 OK") != NULL);
            
            int has_gzip = (strstr(args[i].response, "Content-Encoding: gzip") != NULL) || 
                           (strstr(args[i].response, "content-encoding: gzip") != NULL);
            
            TEST_ASSERT(has_gzip);
            free(args[i].response);
        }
    }
}

// --- Runner ---

void run_server_tests() {
    printf("=== General Server Suite ===\n");
    printf("Note: Ensure the server is running on %s:%d before starting tests.\n\n", SERVER_IP, SERVER_PORT);

    // Basic Connectivity
    RUN_TEST(test_server_connectivity, "Check if server is reachable");

    // Standard Endpoints
    RUN_TEST(test_root_page_load,   "GET / (Root)");
    RUN_TEST(test_home_page,        "GET /home (Check content)");
    RUN_TEST(test_hello_page,       "GET /hello (Check content)");
    RUN_TEST(test_gzip_response_framing, "Gzip response framing and chunks");

    // Error Handling
    RUN_TEST(test_not_found_404,    "GET /nonexistent (Expect 404)");
    RUN_TEST(test_not_implemented_501, "DELETE / (Expect 501/405)");

    // Advanced Features
    RUN_TEST(test_keep_alive,       "Keep-Alive: Multiple reqs on one socket");
    RUN_TEST(test_fragmented_request, "Fragmented request delivery");
    RUN_TEST(test_pipelining,        "Pipelined requests and responses");
    RUN_TEST(test_head_response,     "HEAD response has no body");
    // RUN_TEST(test_multithread_load, "Concurrency: 10 simultaneous requests");
    // RUN_TEST(test_gzip_concurrency, "Concurrency: GZIP requests");
}
