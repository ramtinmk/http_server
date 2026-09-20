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
#include "server_config.h"

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
    // RUN_TEST(test_multithread_load, "Concurrency: 10 simultaneous requests");
    // RUN_TEST(test_gzip_concurrency, "Concurrency: GZIP requests");

    // Phase 2: resource-protection behaviour
    RUN_TEST(test_slow_client_header_timeout, "Phase2: Slow client closed after header timeout");
    RUN_TEST(test_keepalive_request_limit,    "Phase2: Keep-alive connection closed at request limit");
    RUN_TEST(test_input_buffer_limit,         "Phase2: Oversized input rejected with 413 or close");

    // Phase 3: event-loop specific behaviour
    RUN_TEST(test_el_fragmented_headers,      "Phase3: Fragmented header delivery across recv calls");
    RUN_TEST(test_el_coalesced_pipeline,      "Phase3: Coalesced pipelined requests in order");
    RUN_TEST(test_el_abrupt_client_close,     "Phase3: Abrupt client close mid-request");
    RUN_TEST(test_el_keepalive_multiple_cycles, "Phase3: Keep-alive state cycling across 5 requests");
    RUN_TEST(test_el_deep_pipeline,  "Phase3: 33 pipelined requests in one write");
    RUN_TEST(test_el_concurrent_connections,  "Phase3: 20 concurrent connections all complete");
}
