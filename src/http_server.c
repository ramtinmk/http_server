#include "http_server.h"
#include "ring_buffer.h"
#include <signal.h> // Required for signal handling
#include <sys/sendfile.h> // Required for sendfile

#define IDLE_TIMEOUT_SEC 60
#define ZLIB_CHUNK_SIZE 16384

const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");
// Supported methods
const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

// --- Helper wrapper for sending data safely ---
ssize_t send_data(int sockfd, const void *buf, size_t len) {
    // MSG_NOSIGNAL prevents SIGPIPE if the client closed the connection
    return send(sockfd, buf, len, MSG_NOSIGNAL);
}

int create_server_socket(void)
{
    // --- CRITICAL FIX: Ignore SIGPIPE ---
    // Without this, writing to a closed socket crashes the server process.
    signal(SIGPIPE, SIG_IGN);

    int server_socket;
    struct sockaddr_in server_addr;

    // Create socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind socket
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Start listening
    if (listen(server_socket, BACKLOG) == -1)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_socket;
}

void sigchld_handler(int sig)
{
    (void)sig; // Silence unused parameter warning
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void parse_request_line(char *line, HTTPRequest *req)
{
    if (!line)
        return; // Check for NULL line
    char *method_end = strchr(line, ' ');
    if (!method_end)
        return;
    *method_end = '\0';
    strncpy(req->method, line, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';

    char *path_start = method_end + 1;
    char *path_end = strchr(path_start, ' ');
    if (!path_end)
        return;
    *path_end = '\0';
    strncpy(req->path, path_start, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';
}

void print_http_request(const HTTPRequest *req)
{
    printf("Parsed HTTP Request:\n");
    printf("  Method: %s\n", req->method);
    printf("  Path: %s\n", req->path);
    printf("  Headers:\n");
    for (int i = 0; i < req->header_count; i++)
    {
        printf("    %s: %s\n", req->headers[i][0], req->headers[i][1]);
    }
}

static void parse_header_line(char *line, HTTPRequest *req)
{
    if (!line)
        return; // Check for NULL line
    if (req->header_count >= 32)
        return; // Max headers reached

    char *colon = strchr(line, ':');
    if (!colon)
        return;
    *colon = '\0';
    char *value = colon + 1;

    // Trim leading whitespace from value
    while (*value == ' ' || *value == '\t')
        value++;

    // Trim trailing whitespace (like \r) from value if present
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && (*value_end == '\r' || *value_end == '\n' || *value_end == ' ' || *value_end == '\t'))
    {
        *value_end = '\0';
        value_end--;
    }

    strncpy(req->headers[req->header_count][0], line, 255);
    req->headers[req->header_count][0][255] = '\0';
    strncpy(req->headers[req->header_count][1], value, 255);
    req->headers[req->header_count][1][255] = '\0';
    req->header_count++;

    // --- Check for Accept-Encoding header ---
    if (strcasecmp(line, "Accept-Encoding") == 0)
    {
        if (strstr(value, "gzip") != NULL)
        {
            req->accepts_gzip = 1;
        }
    }

    // Check for Connection: close header
    if (strcasecmp(line, "Connection") == 0)
    {
        if (strcasecmp(value, "close") == 0)
        {
            req->keep_alive = 0;
        }
    }
}

void send_error_response(int client_socket, const char *response)
{
    if (send_data(client_socket, response, strlen(response)) < 0)
    {
        perror("write error response");
    }
}

int method_is_supported(const char *method)
{
    for (int i = 0; i < SUPPORTED_METHOD_COUNT; i++)
    {
        if (strcmp(method, SUPPORTED_METHODS[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void *worker_thread_function(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    while (1)
    {
        Task *task = get_task_from_queue(pool);
        if (task == NULL)
        {
            break;
        }

        int client_socket = task->client_socket;
        task_free(pool->task_pool, task);

        handle_client(client_socket, pool->buffer_pool);

        close(client_socket); 
    }

    pthread_exit(NULL);
    return NULL;
}

void handle_client(int client_socket, BufferPool *bp)
{
    RingBuffer *request_rb = buffer_acquire(bp);
    if (!request_rb)
    {
        perror("Failed to create request ring buffer");
        return;
    }

    int keep_alive_connection = 1; 
    int client_closed_flag = 0;    

    // Main connection loop
    while (keep_alive_connection && !client_closed_flag)
    {
        HTTPRequest request;
        struct timeval tv;
        tv.tv_sec = IDLE_TIMEOUT_SEC;
        tv.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);

        // --- Wait for data or timeout ---
        int select_result = select(client_socket + 1, &readfds, NULL, NULL, &tv);

        if (select_result == -1)
        {
            if (errno == EINTR)
                continue; 
            perror("select in handle_client");
            keep_alive_connection = 0; 
            break;
        }
        else if (select_result == 0)
        {
            // Timeout
            keep_alive_connection = 0; 
            break;
        }

        // --- Socket is ready, read data ---
        char temp_buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(client_socket, temp_buffer, sizeof(temp_buffer));

        if (bytes_read < 0)
        {
            if (errno == EINTR) continue; 
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; 
            perror("read in handle_client");
            keep_alive_connection = 0; 
            break;
        }
        else if (bytes_read == 0)
        {
            client_closed_flag = 1;
        }
        else
        {
            size_t written = ring_buffer_write(request_rb, temp_buffer, bytes_read);
            if (written < (size_t)bytes_read)
            {
                fprintf(stderr, "Buffer overflow on socket %d\n", client_socket);
                send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Server buffer capacity exceeded"));
                keep_alive_connection = 0;
                break; 
            }
        }

        // --- Process all complete requests currently in the buffer ---
        RequestStatus status = BUFFER_EMPTY; 
        do
        {
            status = process_single_request(client_socket, request_rb, &request, &keep_alive_connection, client_closed_flag);

            if (status == REQUEST_PARSE_ERROR || status == REQUEST_PROCESS_ERROR)
            {
                keep_alive_connection = 0;
            }

            if (status == CLIENT_CONNECTION_CLOSED)
            {
                keep_alive_connection = 0; 
            }

        } while (status == REQUEST_PROCESSED_OK && keep_alive_connection);

    } 

    buffer_release(bp, request_rb);
}

static RequestStatus process_single_request(int client_socket, RingBuffer *request_rb, HTTPRequest *request, int *keep_alive_connection, int client_closed_flag)
{
    char line_buffer[BUFFER_SIZE]; 

    // --- Save buffer state for potential rollback ---
    size_t initial_rb_size = ring_buffer_get_size(request_rb);
    size_t initial_rb_tail = request_rb->tail;

    if (initial_rb_size == 0)
    {
        return client_closed_flag ? CLIENT_CONNECTION_CLOSED : BUFFER_EMPTY;
    }

    // --- 1. Parse Request Line ---
    char *request_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
    if (!request_line)
    {
        if (client_closed_flag)
        {
            *keep_alive_connection = 0;
            return REQUEST_PARSE_ERROR; 
        }
        return NEED_MORE_DATA;
    }

    memset(request, 0, sizeof(HTTPRequest));
    request->keep_alive = 1; 
    request->accepts_gzip = 0;

    parse_request_line(request_line, request);

    if (strlen(request->method) == 0 || strlen(request->path) == 0)
    {
        send_error_response(client_socket, BAD_REQUEST_400);
        *keep_alive_connection = 0; 
        return REQUEST_PARSE_ERROR;
    }

    if (!method_is_supported(request->method))
    {
        send_error_response(client_socket, NOT_IMPLEMENTED_501);
        *keep_alive_connection = 0;
        return REQUEST_PROCESS_ERROR; 
    }

    // --- 2. Parse Headers ---
    request->header_count = 0;
    while (1)
    {
        char *header_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
        if (!header_line)
        {
            if (client_closed_flag)
            {
                send_error_response(client_socket, BAD_REQUEST_400); 
                *keep_alive_connection = 0;
                return REQUEST_PARSE_ERROR; 
            }
            // Rollback
            request_rb->tail = initial_rb_tail;
            request_rb->size = initial_rb_size;
            return NEED_MORE_DATA;
        }
        if (header_line[0] == '\0')
        {
            break; 
        }
        parse_header_line(header_line, request);
    }

    *keep_alive_connection = request->keep_alive;

    // --- 3. Process Request ---
    char filepath[1024];
    long file_size = 0;
    int file_fd = -1;
    const char *content_type = "text/html"; 

    if (strcmp(request->path, "/home") == 0 || strcmp(request->path, "/") == 0)
    {
        snprintf(filepath, sizeof(filepath), "home.html");
    }
    else if (strcmp(request->path, "/hello") == 0)
    {
        snprintf(filepath, sizeof(filepath), "hello.html");
    }
    else
    {
        send_error_response(client_socket, NOT_FOUND_404);
        return REQUEST_PROCESSED_OK; 
    }

    file_fd = open(filepath, O_RDONLY);
    if (file_fd == -1)
    {
        send_error_response(client_socket, NOT_FOUND_404);
        return REQUEST_PROCESSED_OK; 
    }

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) == -1)
    {
        close(file_fd);
        send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Error accessing file details"));
        *keep_alive_connection = 0; 
        return REQUEST_PROCESS_ERROR;
    }
    file_size = file_stat.st_size;

    int use_gzip = request->accepts_gzip && (strcmp(request->method, "HEAD") != 0);

    // --- 4. Send Response Headers ---
    char response_headers[BUFFER_SIZE];
    snprintf(response_headers, sizeof(response_headers),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Server: SimpleHTTPServer/0.2\r\n" 
             "Connection: %s\r\n",
             content_type,
             *keep_alive_connection ? "keep-alive" : "close");

    if (use_gzip)
    {
        strncat(response_headers, "Content-Encoding: gzip\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
        strncat(response_headers, "Transfer-Encoding: chunked\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
    }
    else
    {
        char length_header[64];
        snprintf(length_header, sizeof(length_header), "Content-Length: %ld\r\n", file_size);
        strncat(response_headers, length_header, sizeof(response_headers) - strlen(response_headers) - 1);
    }
    strncat(response_headers, "\r\n", sizeof(response_headers) - strlen(response_headers) - 1);

    if (send_data(client_socket, response_headers, strlen(response_headers)) < 0)
    {
        perror("send response headers");
        close(file_fd);
        *keep_alive_connection = 0; 
        return REQUEST_PROCESS_ERROR;
    }

    // --- 5. Send Response Body ---
    RequestStatus body_status = REQUEST_PROCESSED_OK;
    if (strcmp(request->method, "HEAD") != 0)
    {
        if (use_gzip)
        {
            // --- GZIP BLOCK START ---
            z_stream strm;
            unsigned char in_buf[ZLIB_CHUNK_SIZE];
            unsigned char out_buf[ZLIB_CHUNK_SIZE];
            int z_ret, flush;
            ssize_t have;

            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            z_ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
            
            if (z_ret != Z_OK)
            {
                fprintf(stderr, "deflateInit failed: %d\n", z_ret);
                body_status = REQUEST_PROCESS_ERROR;
                *keep_alive_connection = 0;
                goto cleanup_fd; // Safely close FD
            }

            do
            {
                ssize_t bytes_read_file = read(file_fd, in_buf, ZLIB_CHUNK_SIZE);
                if (bytes_read_file < 0)
                {
                    perror("read file for compression");
                    deflateEnd(&strm);
                    body_status = REQUEST_PROCESS_ERROR;
                    *keep_alive_connection = 0;
                    goto cleanup_fd;
                }

                strm.avail_in = bytes_read_file;
                flush = (bytes_read_file == 0) ? Z_FINISH : Z_NO_FLUSH;
                strm.next_in = in_buf;

                do
                {
                    strm.avail_out = ZLIB_CHUNK_SIZE;
                    strm.next_out = out_buf;
                    z_ret = deflate(&strm, flush);
                    
                    if (z_ret == Z_STREAM_ERROR) {
                        deflateEnd(&strm);
                        body_status = REQUEST_PROCESS_ERROR;
                        *keep_alive_connection = 0;
                        goto cleanup_fd;
                    }

                    have = ZLIB_CHUNK_SIZE - strm.avail_out;
                    if (have > 0)
                    {
                        char chunk_header[32];
                        snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", (size_t)have);
                        
                        if (send_data(client_socket, chunk_header, strlen(chunk_header)) < 0 ||
                            send_data(client_socket, out_buf, have) < 0 ||
                            send_data(client_socket, "\r\n", 2) < 0)
                        {
                            deflateEnd(&strm);
                            body_status = REQUEST_PROCESS_ERROR;
                            *keep_alive_connection = 0;
                            goto cleanup_fd;
                        }
                    }
                } while (strm.avail_out == 0);

            } while (flush != Z_FINISH);

            // Send final zero-length chunk
            if (send_data(client_socket, "0\r\n\r\n", 5) < 0)
            {
                body_status = REQUEST_PROCESS_ERROR;
                *keep_alive_connection = 0;
            }
            deflateEnd(&strm);
            // --- GZIP BLOCK END ---
        }
        else
        {
            // --- SENDFILE BLOCK ---
            off_t offset = 0;
            // sendfile handling
            // Note: sendfile may fail with EPIPE if client closed, but we handled SIGPIPE globally
            ssize_t sent_bytes = sendfile(client_socket, file_fd, &offset, file_size);
            if (sent_bytes == -1)
            {
                // It's normal for client to disconnect during transfer
                // perror("sendfile"); 
                *keep_alive_connection = 0; 
                body_status = REQUEST_PROCESS_ERROR;
            }
        }
    } 

cleanup_fd:
    close(file_fd); 
    return body_status; 
}