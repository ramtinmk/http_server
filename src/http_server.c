#include "http_server.h"
#include "ring_buffer.h"



#define IDLE_TIMEOUT_SEC 60


const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");
// Supported methods
const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;



int create_server_socket(void)
{
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
        // Simple check for "gzip" substring. A more robust parser
        // might handle quality values (q=).
        if (strstr(value, "gzip") != NULL)
        {
            req->accepts_gzip = 1;
            printf("Client accepts gzip encoding.\n"); // Debug log
        }
    }

    // Check for Connection: close header (existing logic)
    if (strcasecmp(line, "Connection") == 0)
    {
        if (strcasecmp(value, "close") == 0)
        {
            req->keep_alive = 0;
        }
    }
}

// New helper functions
void send_error_response(int client_socket, const char *response)
{
    if (write(client_socket, response, strlen(response)) < 0)
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
            // Null task means shutdown signal, thread should exit
            break;
        }

        int client_socket = task->client_socket;
        // free(task); // Free task structure after getting client socket
        task_free(pool->task_pool, task);

        handle_client(client_socket, pool->buffer_pool);

        close(client_socket); // Close client socket after handling

        // Example of optional delay (for demonstration purposes only, remove in production)
        // sleep(1);
    }

    pthread_exit(NULL);
    return NULL; // Never reached, but good practice to include
}


void handle_client(int client_socket, BufferPool *bp)
{
    // RingBuffer *request_rb = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY); // Use defined initial capacity
    RingBuffer *request_rb = buffer_acquire(bp);
    if (!request_rb)
    {
        perror("Failed to create request ring buffer");
        // Can't send 500 easily here as buffer failed. Just close.
        return;
    }
    // response_rb is not used in this refactor, can be removed if only used for requests
    // RingBuffer *response_rb = ring_buffer_create(BUFFER_SIZE); ... free(response_rb) ...

    int keep_alive_connection = 1; // Assume HTTP/1.1 keep-alive initially
    int client_closed_flag = 0;    // Flag to indicate if read() returned 0

    // Main connection loop
    while (keep_alive_connection && !client_closed_flag)
    {
        HTTPRequest request; // Request struct reused for each request in pipeline
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
                continue; // Interrupted by signal, retry select
            perror("select in handle_client");
            fprintf(stderr, "handle_client: select() error on socket %d, closing.\n", client_socket);
            keep_alive_connection = 0; // Ensure exit
            break;
        }
        else if (select_result == 0)
        {
            // Timeout occurred
            fprintf(stderr, "handle_client: Timeout on socket %d after %d seconds. Closing connection.\n", client_socket, IDLE_TIMEOUT_SEC);
            keep_alive_connection = 0; // Ensure exit
            break;
        }

        // --- Socket is ready, read data ---
        char temp_buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(client_socket, temp_buffer, sizeof(temp_buffer));

        if (bytes_read < 0)
        {
            if (errno == EINTR)
                continue; // Interrupted, retry select/read loop
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; // Should not happen after select, but retry
            perror("read in handle_client");
            fprintf(stderr, "handle_client: read() error on socket %d, closing.\n", client_socket);
            keep_alive_connection = 0; // Ensure exit
            break;
        }
        else if (bytes_read == 0)
        {
            // Client closed connection gracefully
            fprintf(stderr, "handle_client: Client on socket %d closed connection (read returned 0).\n", client_socket);
            client_closed_flag = 1;
            // Don't break yet, process any remaining data in the buffer
        }
        else
        {
            // Write data to ring buffer (dynamic resize handled internally)
            size_t written = ring_buffer_write(request_rb, temp_buffer, bytes_read);
            fprintf(stderr, "handle_client: Wrote %zu bytes to request ring buffer (socket %d, capacity %zu, size %zu)\n",
                    written, client_socket, ring_buffer_get_capacity(request_rb), ring_buffer_get_size(request_rb));
            if (written < (size_t)bytes_read)
            {
                fprintf(stderr, "handle_client: Error writing all read data to ring buffer on socket %d. Closing.\n", client_socket);
                send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Server buffer capacity exceeded"));
                keep_alive_connection = 0;
                break; // Cannot proceed reliably
            }
        }

        // --- Process all complete requests currently in the buffer ---
        RequestStatus status = BUFFER_EMPTY; // Initial status before processing loop
        do
        {
            // Pass current keep_alive state and client_closed flag
            status = process_single_request(client_socket, request_rb, &request, &keep_alive_connection, client_closed_flag);

            // If an error occurred during processing, the connection should be closed.
            if (status == REQUEST_PARSE_ERROR || status == REQUEST_PROCESS_ERROR)
            {
                fprintf(stderr, "handle_client: Error processing request on socket %d. Closing connection.\n", client_socket);
                keep_alive_connection = 0;
            }

            // If client closed and buffer is now empty/handled, ensure we exit main loop
            if (status == CLIENT_CONNECTION_CLOSED)
            {
                fprintf(stderr, "handle_client: Client connection closed and buffer processed for socket %d.\n", client_socket);
                keep_alive_connection = 0; // Ensure exit from outer loop
            }

        } while (status == REQUEST_PROCESSED_OK && keep_alive_connection);
        // Loop continues as long as we successfully process requests and keep_alive is desired.
        // Loop breaks if:
        // - status is NEED_MORE_DATA (go back to select)
        // - status is BUFFER_EMPTY (go back to select)
        // - status indicates error or client close (keep_alive_connection set to 0)
        // - keep_alive_connection becomes false (e.g., "Connection: close" received)

    } // End while(keep_alive_connection && !client_closed_flag)

    fprintf(stderr, "handle_client: Finished handling client on socket %d. Final keep-alive state: %d.\n",
            client_socket, keep_alive_connection);

    // Cleanup
    // ring_buffer_free(request_rb);
    buffer_release(bp, request_rb);
    // ring_buffer_free(response_rb); // If response_rb was used
}




// /**
//  * @brief Attempts to parse and process a single HTTP request from the ring buffer.
//  *
//  * Reads from the ring buffer, parses the request line and headers. If a full
//  * request is parsed, it determines the resource, sends appropriate headers
//  * (handling gzip and keep-alive), and sends the response body (gzipped or plain).
//  *
//  * @param client_socket The client's socket file descriptor.
//  * @param request_rb The ring buffer containing request data.
//  * @param request A pointer to an HTTPRequest struct to be filled.
//  * @param keep_alive_connection A pointer to the flag indicating if the connection
//  *                              should be kept alive. This function may set it to 0
//  *                              on errors or if "Connection: close" is received.
//  * @param client_closed_flag A flag indicating if read() previously returned 0.
//  * @return RequestStatus indicating the outcome of the attempt.
//  */
static RequestStatus process_single_request(int client_socket, RingBuffer *request_rb, HTTPRequest *request, int *keep_alive_connection, int client_closed_flag)
{
    char line_buffer[BUFFER_SIZE]; // Temporary buffer for reading lines

    // --- Save buffer state for potential rollback ---
    size_t initial_rb_size = ring_buffer_get_size(request_rb);
    size_t initial_rb_tail = request_rb->tail;

    // If the buffer is empty, nothing to process right now.
    if (initial_rb_size == 0)
    {
        return client_closed_flag ? CLIENT_CONNECTION_CLOSED : BUFFER_EMPTY;
    }

    // --- 1. Parse Request Line ---
    char *request_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
    if (!request_line)
    {
        // Not enough data for a complete request line yet.
        if (client_closed_flag)
        {
            fprintf(stderr, "process_single_request: Connection closed by client with incomplete request line on socket %d.\n", client_socket);
            *keep_alive_connection = 0;
            return REQUEST_PARSE_ERROR; // Treat incomplete request on close as error
        }
        // Need more data, rollback not necessary as readline didn't consume if NULL
        return NEED_MORE_DATA;
    }

    // Reset request struct for this request
    memset(request, 0, sizeof(HTTPRequest));
    request->keep_alive = 1; // Assume HTTP/1.1 keep-alive default
    request->accepts_gzip = 0;

    parse_request_line(request_line, request);

    size_t debug_size = ring_buffer_get_size(request_rb);
    char debug_peek_buf[256];
    size_t debug_peeked = ring_buffer_peek(request_rb, debug_peek_buf, debug_size < 255 ? debug_size : 255);
    if (debug_peeked > 0)
        debug_peek_buf[debug_peeked] = '\0';
    else
        debug_peek_buf[0] = '\0';
    fprintf(stderr, "DEBUG SERVER: Socket %d: Buffer content AFTER req line read (size %zu, peeked %zu): [[%s]]\n",
            client_socket, debug_size, debug_peeked, debug_peek_buf);
    if (strlen(request->method) == 0 || strlen(request->path) == 0)
    {
        fprintf(stderr, "process_single_request: Failed to parse request line: '%s'. Sending 400.\n", line_buffer);
        send_error_response(client_socket, BAD_REQUEST_400);
        *keep_alive_connection = 0; // Bad request, close connection
        // Don't try to process rest of buffer for this connection
        return REQUEST_PARSE_ERROR;
    }

    if (!method_is_supported(request->method))
    {
        fprintf(stderr, "process_single_request: Unsupported method '%s'. Sending 501.\n", request->method);
        send_error_response(client_socket, NOT_IMPLEMENTED_501);
        // Honor explicit "Connection: close" if sent with unsupported method, otherwise default close
        // (We haven't parsed headers yet, so can't check Connection header here easily. Safest is to close.)
        *keep_alive_connection = 0;
        return REQUEST_PROCESS_ERROR; // Treat as processing error leading to close
    }

    // --- 2. Parse Headers ---
    request->header_count = 0;
    int header_parse_complete = 0;
    while (1)
    {
        char *header_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
        if (!header_line)
        {
            // Not enough data for the next header line or the final empty line.
            if (client_closed_flag)
            {
                fprintf(stderr, "process_single_request: Connection closed by client mid-headers on socket %d.\n", client_socket);
                send_error_response(client_socket, BAD_REQUEST_400); // Send 400 for incomplete request
                *keep_alive_connection = 0;
                return REQUEST_PARSE_ERROR; // Treat incomplete request on close as error
            }
            // Need more data. Rollback buffer state to before request line was read.
            request_rb->tail = initial_rb_tail;
            request_rb->size = initial_rb_size;
            // fprintf(stderr, "DEBUG: Rolling back buffer state for socket %d. Tail=%zu, Size=%zu\n", client_socket, request_rb->tail, request_rb->size);
            return NEED_MORE_DATA;
        }
        if (header_line[0] == '\0')
        {
            // Empty line indicates end of headers
            header_parse_complete = 1;
            break; // Exit header parsing loop
        }
        // parse_header_line now also checks Accept-Encoding and Connection
        parse_header_line(header_line, request);
    }

    // Update keep_alive based on parsed "Connection: close" header
    *keep_alive_connection = request->keep_alive;

    // --- 3. Process Request & Prepare Response Data ---
    // TODO: Handle request body if needed (e.g., for POST). Simple server ignores it.

    char filepath[1024];
    long file_size = 0;
    int file_fd = -1;
    const char *content_type = "text/html"; // Default

    // Simple routing logic
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
        fprintf(stderr, "process_single_request: Path '%s' not found. Sending 404.\n", request->path);
        send_error_response(client_socket, NOT_FOUND_404);
        // Keep-alive status is already set based on headers, just return OK status for error response sent
        return REQUEST_PROCESSED_OK; // Indicate request handled (by sending 404)
    }

    file_fd = open(filepath, O_RDONLY);
    if (file_fd == -1)
    {
        perror("open");
        fprintf(stderr, "process_single_request: File '%s' not found/error opening. Sending 404.\n", filepath);
        send_error_response(client_socket, NOT_FOUND_404);
        return REQUEST_PROCESSED_OK; // Indicate request handled (by sending 404)
    }

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) == -1)
    {
        perror("fstat");
        close(file_fd);
        fprintf(stderr, "process_single_request: Error getting file stats for '%s'. Sending 500.\n", filepath);
        send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Error accessing file details"));
        *keep_alive_connection = 0; // Internal error, close connection
        return REQUEST_PROCESS_ERROR;
    }
    file_size = file_stat.st_size;

    int use_gzip = request->accepts_gzip && (strcmp(request->method, "HEAD") != 0);
    printf("DEBUG SERVER: Socket %d: accepts_gzip=%d, method='%s', calculated use_gzip=%d\n",
           client_socket, request->accepts_gzip, request->method, use_gzip);
    // --- 4. Send Response Headers ---
    char response_headers[BUFFER_SIZE];
    // Start with base headers
    snprintf(response_headers, sizeof(response_headers),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Server: SimpleHTTPServer/0.2\r\n" // Optional Server header
             "Connection: %s\r\n",
             content_type,
             *keep_alive_connection ? "keep-alive" : "close");

    if (use_gzip)
    {
        // Append headers for gzip + chunked encoding
        strncat(response_headers, "Content-Encoding: gzip\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
        strncat(response_headers, "Transfer-Encoding: chunked\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
    }
    else
    {
        // Append Content-Length header for non-gzipped responses
        char length_header[64];
        snprintf(length_header, sizeof(length_header), "Content-Length: %ld\r\n", file_size);
        strncat(response_headers, length_header, sizeof(response_headers) - strlen(response_headers) - 1);
    }
    // Append the final CRLF marking the end of headers
    strncat(response_headers, "\r\n", sizeof(response_headers) - strlen(response_headers) - 1);

    // Send Headers
    if (write(client_socket, response_headers, strlen(response_headers)) < 0)
    {
        perror("write response headers");
        close(file_fd);
        *keep_alive_connection = 0; // Error writing, force close
        return REQUEST_PROCESS_ERROR;
    }

    // --- 5. Send Response Body (if not HEAD request) ---
    RequestStatus body_status = REQUEST_PROCESSED_OK;
    if (strcmp(request->method, "HEAD") != 0)
    {
        if (use_gzip)
        {
            // --- Send Gzipped Body using Chunked Transfer Encoding ---
            z_stream strm;
            unsigned char in_buf[ZLIB_CHUNK_SIZE];
            unsigned char out_buf[ZLIB_CHUNK_SIZE];
            int z_ret, flush;
            ssize_t have;

            // Initialize zlib stream
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            z_ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
            if (z_ret != Z_OK)
            {
                fprintf(stderr, "process_single_request: deflateInit failed: %d\n", z_ret);
                close(file_fd);
                *keep_alive_connection = 0;
                body_status = REQUEST_PROCESS_ERROR;
                goto cleanup_fd; // Use goto locally for FD cleanup
            }

            // Compression loop
            do
            {
                ssize_t bytes_read_file = read(file_fd, in_buf, ZLIB_CHUNK_SIZE);
                if (bytes_read_file < 0)
                {
                    perror("read file for compression");
                    (void)deflateEnd(&strm);
                    close(file_fd);
                    *keep_alive_connection = 0;
                    body_status = REQUEST_PROCESS_ERROR;
                    goto cleanup_logic; // Skip FD cleanup as it's done
                }
                strm.avail_in = bytes_read_file;
                flush = (bytes_read_file == 0) ? Z_FINISH : Z_NO_FLUSH;
                strm.next_in = in_buf;

                do
                {
                    strm.avail_out = ZLIB_CHUNK_SIZE;
                    strm.next_out = out_buf;
                    z_ret = deflate(&strm, flush);
                    if (z_ret == Z_STREAM_ERROR)
                    {
                        fprintf(stderr, "process_single_request: deflate error: %d\n", z_ret);
                        (void)deflateEnd(&strm);
                        close(file_fd);
                        *keep_alive_connection = 0;
                        body_status = REQUEST_PROCESS_ERROR;
                        goto cleanup_logic;
                    }
                    have = ZLIB_CHUNK_SIZE - strm.avail_out;
                    if (have > 0)
                    {
                        char chunk_header[32];
                        snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", (size_t)have);
                        if (write(client_socket, chunk_header, strlen(chunk_header)) < 0 ||
                            write(client_socket, out_buf, have) < 0 ||
                            write(client_socket, "\r\n", 2) < 0)
                        {
                            perror("write gzip chunk data");
                            (void)deflateEnd(&strm);
                            close(file_fd);
                            *keep_alive_connection = 0;
                            body_status = REQUEST_PROCESS_ERROR;
                            goto cleanup_logic;
                        }
                    }
                } while (strm.avail_out == 0);

                if (strm.avail_in != 0)
                { // Should not happen with Z_NO_FLUSH/Z_FINISH logic
                    fprintf(stderr, "process_single_request: deflate error - input not fully consumed?\n");
                    (void)deflateEnd(&strm);
                    close(file_fd);
                    *keep_alive_connection = 0;
                    body_status = REQUEST_PROCESS_ERROR;
                    goto cleanup_logic;
                }
            } while (flush != Z_FINISH);

            if (z_ret != Z_STREAM_END)
            {
                fprintf(stderr, "process_single_request: deflate did not end stream correctly: %d\n", z_ret);
                // May not be fatal, but log it.
            }

            // Send final zero-length chunk
            if (write(client_socket, "0\r\n\r\n", 5) < 0)
            {
                perror("write final chunk");
                *keep_alive_connection = 0; // Error, close connection
                body_status = REQUEST_PROCESS_ERROR;
                // deflateEnd and close(fd) still need to happen
            }
            (void)deflateEnd(&strm);
        }
        else
        {
            // --- Send Uncompressed Body using sendfile ---
            off_t offset = 0;
            ssize_t sent_bytes = sendfile(client_socket, file_fd, &offset, file_size);
            if (sent_bytes == -1)
            {
                perror("sendfile");
                *keep_alive_connection = 0; // Error sending, close connection
                body_status = REQUEST_PROCESS_ERROR;
                // close(fd) still needs to happen
            }
            else if (sent_bytes != file_size)
            {
                fprintf(stderr, "process_single_request: Warning - sendfile sent %zd bytes, expected %ld for %s\n", sent_bytes, file_size, filepath);
                // Client might have closed connection early. Don't necessarily force close server side yet.
            }
        }
    } // End if not HEAD
cleanup_fd:
    close(file_fd); // Close the file descriptor

cleanup_logic:
    return body_status; // Return status from body sending (or OK if HEAD)
}