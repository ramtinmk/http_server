/*
 * Simple HTTP Server for Educational Purposes
 *
 * This implementation demonstrates basic socket programming
 * and HTTP protocol handling. Not recommended for production use.
 */

#include "http_server.h"

#define INITIAL_RING_BUFFER_CAPACITY BUFFER_SIZE

void sigchld_handler(int sig);
static void parse_request_line(char *line, HTTPRequest *req);
static void parse_header_line(char *line, HTTPRequest *req);

ThreadPool *create_thread_pool(int pool_size);
void destroy_thread_pool(ThreadPool *pool);
void add_task_to_queue(ThreadPool *pool, int client_socket);
Task *get_task_from_queue(ThreadPool *pool); // Or Task* get_task_from_queue(...) depending on task structure return
void *worker_thread_function(void *arg);

// Add this enum definition near the top or in http_server.h if preferred
typedef enum {
    REQUEST_PROCESSED_OK,      // Successfully processed one request
    NEED_MORE_DATA,          // Parsed partial request, need more data from socket
    REQUEST_PARSE_ERROR,     // Malformed request or headers
    REQUEST_PROCESS_ERROR,   // Error during file handling, compression, or sending response
    CLIENT_CONNECTION_CLOSED, // Client closed connection (read returned 0) and buffer is handled
    BUFFER_EMPTY             // Ring buffer was empty, nothing to process
} RequestStatus;


// Error responses
const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");

// Supported methods
const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

// Helper functions
int validate_request(HTTPRequest *req);
static RequestStatus process_single_request(int client_socket, RingBuffer *request_rb, HTTPRequest *request, int *keep_alive_connection, int client_closed_flag);
// --- Thread Pool Configuration ---
#define THREAD_POOL_SIZE 16

#define IDLE_TIMEOUT_SEC 60

int main()
{
    int server_socket, client_socket;
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    // --- Remove SIGCHLD handler ---
    /*
    // Set up SIGCHLD handler to prevent zombie processes
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    */

    // --- Initialize Thread Pool ---
    ThreadPool *thread_pool = create_thread_pool(THREAD_POOL_SIZE);
    if (thread_pool == NULL)
    {
        fprintf(stderr, "Failed to create thread pool\n");
        exit(EXIT_FAILURE);
    }
    printf("Thread pool initialized with %d threads.\n", THREAD_POOL_SIZE);

    // Create server socket
    server_socket = create_server_socket();
    printf("Server listening on port %d...\n", PORT);

    while (1)
    {
        // Accept incoming connection
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
        if (client_socket == -1)
        {
            perror("accept");
            continue;
        }

        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        // --- Submit task to thread pool instead of forking ---
        add_task_to_queue(thread_pool, client_socket);

        // --- Remove fork() child/parent process logic ---
        /*
        pid_t pid = fork();
        if (pid == 0)
        { // Child process
            close(server_socket);
            printf("Client connected: %s:%d\n",
                    inet_ntoa(client_addr.sin_addr),
                    ntohs(client_addr.sin_port));

            handle_client(client_socket);

            close(client_socket);
            exit(EXIT_SUCCESS);
        }
        else if (pid > 0)
        { // Parent process
            close(client_socket);
        }
        else
        {
            perror("fork");
        }
        */
    }

    close(server_socket);

    // --- Destroy Thread Pool before exiting ---
    destroy_thread_pool(thread_pool);
    printf("Thread pool destroyed.\n");

    return 0;
}

// --- Implement Thread Pool Functions ---

ThreadPool *create_thread_pool(int pool_size)
{
    if (pool_size <= 0)
    {
        fprintf(stderr, "Error: Pool size must be greater than 0\n");
        return NULL;
    }

    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool)
    {
        perror("Failed to allocate thread pool");
        return NULL;
    }

    pool->pool_size = pool_size;
    pool->threads = malloc(sizeof(pthread_t) * pool_size);
    if (!pool->threads)
    {
        perror("Failed to allocate thread array");
        free(pool);
        return NULL;
    }

    pool->task_queue_head = NULL;
    pool->task_queue_tail = NULL;
    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0)
    {
        perror("Mutex initialization failed");
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->queue_cond, NULL) != 0)
    {
        perror("Condition variable initialization failed");
        pthread_mutex_destroy(&pool->queue_mutex);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->shutdown = 0;

    for (int i = 0; i < pool_size; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread_function, pool) != 0)
        {
            perror("Failed to create worker thread");
            // In a real-world scenario, you'd want to handle thread creation failure more gracefully,
            // potentially destroying already created threads and pool resources.
            destroy_thread_pool(pool); // Attempt to cleanup partially created pool
            return NULL;
        }
    }

    return pool;
}

void destroy_thread_pool(ThreadPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = 1;                        // Set shutdown flag
    pthread_cond_broadcast(&pool->queue_cond); // Wake up all worker threads
    pthread_mutex_unlock(&pool->queue_mutex);

    for (int i = 0; i < pool->pool_size; i++)
    {
        if (pthread_join(pool->threads[i], NULL) != 0)
        {
            perror("Failed to join worker thread");
        }
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);

    // Free task queue (important to free any remaining tasks if queue wasn't fully processed)
    Task *current_task = pool->task_queue_head;
    while (current_task != NULL)
    {
        Task *next_task = current_task->next;
        free(current_task);
        current_task = next_task;
    }

    free(pool->threads);
    free(pool);
}

void add_task_to_queue(ThreadPool *pool, int client_socket)
{
    if (!pool)
        return;

    Task *new_task = malloc(sizeof(Task));
    if (!new_task)
    {
        perror("Failed to allocate task");
        close(client_socket); // Important: close socket if task allocation fails
        return;
    }
    new_task->client_socket = client_socket;
    new_task->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->task_queue_tail == NULL)
    {
        pool->task_queue_head = new_task;
        pool->task_queue_tail = new_task;
    }
    else
    {
        pool->task_queue_tail->next = new_task;
        pool->task_queue_tail = new_task;
    }

    pthread_cond_signal(&pool->queue_cond); // Signal a worker thread that a task is available
    pthread_mutex_unlock(&pool->queue_mutex);
}

Task *get_task_from_queue(ThreadPool *pool)
{
    if (!pool)
        return NULL;

    pthread_mutex_lock(&pool->queue_mutex);

    // Wait while queue is empty and server is not shutting down
    while (pool->task_queue_head == NULL && !pool->shutdown)
    {
        pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
    }

    if (pool->shutdown && pool->task_queue_head == NULL)
    {
        pthread_mutex_unlock(&pool->queue_mutex);
        return NULL; // Signal for thread to exit
    }

    Task *task = pool->task_queue_head;
    if (task != NULL)
    {
        pool->task_queue_head = task->next;
        if (pool->task_queue_head == NULL)
        {
            pool->task_queue_tail = NULL; // Queue became empty
        }
    }

    pthread_mutex_unlock(&pool->queue_mutex);
    return task;
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
        free(task); // Free task structure after getting client socket

        handle_client(client_socket); // Process the client request

        close(client_socket); // Close client socket after handling

        // Example of optional delay (for demonstration purposes only, remove in production)
        // sleep(1);
    }

    pthread_exit(NULL);
    return NULL; // Never reached, but good practice to include
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

static void parse_header_line(char *line, HTTPRequest *req)
{
    if (!line) return; // Check for NULL line
    if (req->header_count >= 32) return; // Max headers reached

    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = '\0';
    char *value = colon + 1;

    // Trim leading whitespace from value
    while (*value == ' ' || *value == '\t') value++;

    // Trim trailing whitespace (like \r) from value if present
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && (*value_end == '\r' || *value_end == '\n' || *value_end == ' ' || *value_end == '\t')) {
        *value_end = '\0';
        value_end--;
    }

    strncpy(req->headers[req->header_count][0], line, 255);
    req->headers[req->header_count][0][255] = '\0';
    strncpy(req->headers[req->header_count][1], value, 255);
    req->headers[req->header_count][1][255] = '\0';
    req->header_count++;

    // --- Check for Accept-Encoding header ---
    if (strcasecmp(line, "Accept-Encoding") == 0) {
        // Simple check for "gzip" substring. A more robust parser
        // might handle quality values (q=).
        if (strstr(value, "gzip") != NULL) {
            req->accepts_gzip = 1;
            printf("Client accepts gzip encoding.\n"); // Debug log
        }
    }

    // Check for Connection: close header (existing logic)
    if (strcasecmp(line, "Connection") == 0) {
        if (strcasecmp(value, "close") == 0) {
            req->keep_alive = 0;
        }
    }
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
/**
 * @brief Main handler for a client connection.
 *
 * Manages the connection lifecycle, reads data into a ring buffer using select()
 * for timeouts, and calls process_single_request in a loop to handle pipelined
 * requests until the connection is closed or an error occurs.
 *
 * @param client_socket The client's socket file descriptor.
 */
void handle_client(int client_socket) {
    RingBuffer *request_rb = ring_buffer_create(INITIAL_RING_BUFFER_CAPACITY); // Use defined initial capacity
    if (!request_rb) {
        perror("Failed to create request ring buffer");
        // Can't send 500 easily here as buffer failed. Just close.
        return;
    }
    // response_rb is not used in this refactor, can be removed if only used for requests
    // RingBuffer *response_rb = ring_buffer_create(BUFFER_SIZE); ... free(response_rb) ...

    int keep_alive_connection = 1; // Assume HTTP/1.1 keep-alive initially
    int client_closed_flag = 0;    // Flag to indicate if read() returned 0

    // Main connection loop
    while (keep_alive_connection && !client_closed_flag) {
        HTTPRequest request; // Request struct reused for each request in pipeline
        struct timeval tv;
        tv.tv_sec = IDLE_TIMEOUT_SEC;
        tv.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds);

        // --- Wait for data or timeout ---
        int select_result = select(client_socket + 1, &readfds, NULL, NULL, &tv);

        if (select_result == -1) {
            if (errno == EINTR) continue; // Interrupted by signal, retry select
            perror("select in handle_client");
            fprintf(stderr, "handle_client: select() error on socket %d, closing.\n", client_socket);
            keep_alive_connection = 0; // Ensure exit
            break;
        } else if (select_result == 0) {
            // Timeout occurred
            fprintf(stderr, "handle_client: Timeout on socket %d after %d seconds. Closing connection.\n", client_socket, IDLE_TIMEOUT_SEC);
            keep_alive_connection = 0; // Ensure exit
            break;
        }

        // --- Socket is ready, read data ---
        char temp_buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(client_socket, temp_buffer, sizeof(temp_buffer));

        if (bytes_read < 0) {
            if (errno == EINTR) continue; // Interrupted, retry select/read loop
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Should not happen after select, but retry
            perror("read in handle_client");
            fprintf(stderr, "handle_client: read() error on socket %d, closing.\n", client_socket);
            keep_alive_connection = 0; // Ensure exit
            break;
        } else if (bytes_read == 0) {
            // Client closed connection gracefully
            fprintf(stderr, "handle_client: Client on socket %d closed connection (read returned 0).\n", client_socket);
            client_closed_flag = 1;
            // Don't break yet, process any remaining data in the buffer
        } else {
            // Write data to ring buffer (dynamic resize handled internally)
            size_t written = ring_buffer_write(request_rb, temp_buffer, bytes_read);
             fprintf(stderr, "handle_client: Wrote %zu bytes to request ring buffer (socket %d, capacity %zu, size %zu)\n",
                        written, client_socket, ring_buffer_get_capacity(request_rb), ring_buffer_get_size(request_rb));
            if (written < (size_t)bytes_read) {
                fprintf(stderr, "handle_client: Error writing all read data to ring buffer on socket %d. Closing.\n", client_socket);
                send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Server buffer capacity exceeded"));
                keep_alive_connection = 0;
                break; // Cannot proceed reliably
            }
        }

        // --- Process all complete requests currently in the buffer ---
        RequestStatus status = BUFFER_EMPTY; // Initial status before processing loop
        do {
            // Pass current keep_alive state and client_closed flag
            status = process_single_request(client_socket, request_rb, &request, &keep_alive_connection, client_closed_flag);

            // If an error occurred during processing, the connection should be closed.
            if (status == REQUEST_PARSE_ERROR || status == REQUEST_PROCESS_ERROR) {
                fprintf(stderr, "handle_client: Error processing request on socket %d. Closing connection.\n", client_socket);
                keep_alive_connection = 0;
            }

            // If client closed and buffer is now empty/handled, ensure we exit main loop
            if (status == CLIENT_CONNECTION_CLOSED) {
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
    ring_buffer_free(request_rb);
    // ring_buffer_free(response_rb); // If response_rb was used
}

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
static RequestStatus process_single_request(int client_socket, RingBuffer *request_rb, HTTPRequest *request, int *keep_alive_connection, int client_closed_flag) {
    char line_buffer[BUFFER_SIZE]; // Temporary buffer for reading lines

    // --- Save buffer state for potential rollback ---
    size_t initial_rb_size = ring_buffer_get_size(request_rb);
    size_t initial_rb_tail = request_rb->tail;

    // If the buffer is empty, nothing to process right now.
    if (initial_rb_size == 0) {
        return client_closed_flag ? CLIENT_CONNECTION_CLOSED : BUFFER_EMPTY;
    }

    // --- 1. Parse Request Line ---
    char *request_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
    if (!request_line) {
        // Not enough data for a complete request line yet.
        if (client_closed_flag) {
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
    if (debug_peeked > 0) debug_peek_buf[debug_peeked] = '\0'; else debug_peek_buf[0] = '\0';
    fprintf(stderr, "DEBUG SERVER: Socket %d: Buffer content AFTER req line read (size %zu, peeked %zu): [[%s]]\n",
            client_socket, debug_size, debug_peeked, debug_peek_buf);
    if (strlen(request->method) == 0 || strlen(request->path) == 0) {
        fprintf(stderr, "process_single_request: Failed to parse request line: '%s'. Sending 400.\n", line_buffer);
        send_error_response(client_socket, BAD_REQUEST_400);
        *keep_alive_connection = 0; // Bad request, close connection
        // Don't try to process rest of buffer for this connection
        return REQUEST_PARSE_ERROR;
    }

    if (!method_is_supported(request->method)) {
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
    while (1) {
        char *header_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
        if (!header_line) {
            // Not enough data for the next header line or the final empty line.
            if (client_closed_flag) {
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
        if (header_line[0] == '\0') {
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
    if (strcmp(request->path, "/home") == 0 || strcmp(request->path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "home.html");
    } else if (strcmp(request->path, "/hello") == 0) {
        snprintf(filepath, sizeof(filepath), "hello.html");
    } else {
        fprintf(stderr, "process_single_request: Path '%s' not found. Sending 404.\n", request->path);
        send_error_response(client_socket, NOT_FOUND_404);
        // Keep-alive status is already set based on headers, just return OK status for error response sent
        return REQUEST_PROCESSED_OK; // Indicate request handled (by sending 404)
    }

    file_fd = open(filepath, O_RDONLY);
    if (file_fd == -1) {
        perror("open");
        fprintf(stderr, "process_single_request: File '%s' not found/error opening. Sending 404.\n", filepath);
        send_error_response(client_socket, NOT_FOUND_404);
        return REQUEST_PROCESSED_OK; // Indicate request handled (by sending 404)
    }

    struct stat file_stat;
    if (fstat(file_fd, &file_stat) == -1) {
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

    if (use_gzip) {
        // Append headers for gzip + chunked encoding
        strncat(response_headers, "Content-Encoding: gzip\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
        strncat(response_headers, "Transfer-Encoding: chunked\r\n", sizeof(response_headers) - strlen(response_headers) - 1);
    } else {
        // Append Content-Length header for non-gzipped responses
        char length_header[64];
        snprintf(length_header, sizeof(length_header), "Content-Length: %ld\r\n", file_size);
        strncat(response_headers, length_header, sizeof(response_headers) - strlen(response_headers) - 1);
    }
    // Append the final CRLF marking the end of headers
    strncat(response_headers, "\r\n", sizeof(response_headers) - strlen(response_headers) - 1);

    // Send Headers
    if (write(client_socket, response_headers, strlen(response_headers)) < 0) {
        perror("write response headers");
        close(file_fd);
        *keep_alive_connection = 0; // Error writing, force close
        return REQUEST_PROCESS_ERROR;
    }

    // --- 5. Send Response Body (if not HEAD request) ---
    RequestStatus body_status = REQUEST_PROCESSED_OK;
    if (strcmp(request->method, "HEAD") != 0) {
        if (use_gzip) {
            // --- Send Gzipped Body using Chunked Transfer Encoding ---
            z_stream strm;
            unsigned char in_buf[ZLIB_CHUNK_SIZE];
            unsigned char out_buf[ZLIB_CHUNK_SIZE];
            int z_ret, flush;
            ssize_t have;

            // Initialize zlib stream
            strm.zalloc = Z_NULL; strm.zfree = Z_NULL; strm.opaque = Z_NULL;
            z_ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
            if (z_ret != Z_OK) {
                fprintf(stderr, "process_single_request: deflateInit failed: %d\n", z_ret);
                close(file_fd);
                *keep_alive_connection = 0;
                body_status = REQUEST_PROCESS_ERROR;
                goto cleanup_fd; // Use goto locally for FD cleanup
            }

            // Compression loop
            do {
                ssize_t bytes_read_file = read(file_fd, in_buf, ZLIB_CHUNK_SIZE);
                if (bytes_read_file < 0) {
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

                do {
                    strm.avail_out = ZLIB_CHUNK_SIZE;
                    strm.next_out = out_buf;
                    z_ret = deflate(&strm, flush);
                    if (z_ret == Z_STREAM_ERROR) {
                        fprintf(stderr, "process_single_request: deflate error: %d\n", z_ret);
                        (void)deflateEnd(&strm);
                        close(file_fd);
                        *keep_alive_connection = 0;
                        body_status = REQUEST_PROCESS_ERROR;
                        goto cleanup_logic;
                    }
                    have = ZLIB_CHUNK_SIZE - strm.avail_out;
                    if (have > 0) {
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

                if (strm.avail_in != 0) { // Should not happen with Z_NO_FLUSH/Z_FINISH logic
                    fprintf(stderr, "process_single_request: deflate error - input not fully consumed?\n");
                     (void)deflateEnd(&strm);
                     close(file_fd);
                     *keep_alive_connection = 0;
                     body_status = REQUEST_PROCESS_ERROR;
                     goto cleanup_logic;
                }
            } while (flush != Z_FINISH);

            if (z_ret != Z_STREAM_END) {
                fprintf(stderr, "process_single_request: deflate did not end stream correctly: %d\n", z_ret);
                 // May not be fatal, but log it.
            }

            // Send final zero-length chunk
            if (write(client_socket, "0\r\n\r\n", 5) < 0) {
                perror("write final chunk");
                *keep_alive_connection = 0; // Error, close connection
                 body_status = REQUEST_PROCESS_ERROR;
                 // deflateEnd and close(fd) still need to happen
            }
            (void)deflateEnd(&strm);

        } else {
            // --- Send Uncompressed Body using sendfile ---
            off_t offset = 0;
            ssize_t sent_bytes = sendfile(client_socket, file_fd, &offset, file_size);
            if (sent_bytes == -1) {
                perror("sendfile");
                 *keep_alive_connection = 0; // Error sending, close connection
                body_status = REQUEST_PROCESS_ERROR;
                 // close(fd) still needs to happen
            } else if (sent_bytes != file_size) {
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

// --- Ring Buffer Function Implementations ---
static int ring_buffer_resize(RingBuffer *rb, size_t new_capacity) {
    if (!rb || new_capacity <= rb->capacity || new_capacity < rb->size) {
        // Invalid arguments or no resize needed/possible
        return -1; // Or maybe 0 if new_capacity <= capacity? Let's stick to -1 for error.
    }

    // Optional: Check against a maximum capacity limit
    /*
    if (new_capacity > MAX_RING_BUFFER_CAPACITY) {
        fprintf(stderr, "Error: RingBuffer resize requested capacity %zu exceeds maximum %d\n",
                new_capacity, MAX_RING_BUFFER_CAPACITY);
        errno = ENOMEM; // Indicate memory limit reached
        return -1;
    }
    */

    char *new_buffer = malloc(new_capacity);
    if (!new_buffer) {
        perror("Failed to allocate memory for RingBuffer resize");
        return -1;
    }

    // Copy data from old buffer to new buffer, linearizing it
    size_t bytes_to_copy_part1 = 0;
    size_t bytes_to_copy_part2 = 0;

    if (rb->size > 0) {
         if (rb->head > rb->tail) {
            // Data is contiguous
            bytes_to_copy_part1 = rb->size;
            memcpy(new_buffer, rb->buffer + rb->tail, bytes_to_copy_part1);
        } else {
            // Data wraps around
            bytes_to_copy_part1 = rb->capacity - rb->tail;
            memcpy(new_buffer, rb->buffer + rb->tail, bytes_to_copy_part1);

            bytes_to_copy_part2 = rb->head;
            memcpy(new_buffer + bytes_to_copy_part1, rb->buffer, bytes_to_copy_part2);
        }
    }


    // Free the old buffer
    free(rb->buffer);

    // Update RingBuffer structure
    rb->buffer = new_buffer;
    rb->capacity = new_capacity;
    rb->tail = 0;        // Data is now linear, starting at index 0
    rb->head = rb->size; // Head is positioned after the last byte of existing data

    // fprintf(stderr, "DEBUG: RingBuffer resized to capacity %zu\n", new_capacity); // Optional debug print

    return 0; // Success
}

RingBuffer *ring_buffer_create(size_t initial_capacity) {
    // Use INITIAL_RING_BUFFER_CAPACITY if initial_capacity is 0 or too small
    if (initial_capacity == 0) initial_capacity = INITIAL_RING_BUFFER_CAPACITY;

    RingBuffer *rb = malloc(sizeof(RingBuffer));
    if (!rb) {
        perror("Failed to allocate RingBuffer struct");
        return NULL;
    }
    rb->buffer = malloc(initial_capacity);
    if (!rb->buffer) {
        perror("Failed to allocate RingBuffer internal buffer");
        free(rb);
        return NULL;
    }
    rb->capacity = initial_capacity;
    ring_buffer_reset(rb); // Initialize head, tail, size
    return rb;
}


// --- Modified ring_buffer_write ---
size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len) {
    if (!rb || !data || data_len == 0)
        return 0;

    size_t available_space = rb->capacity - rb->size;

    // Check if resizing is needed
    if (data_len > available_space) {
        // Calculate new capacity: at least double, but ensure enough space for new data
        size_t needed_capacity = rb->size + data_len;
        size_t new_capacity = rb->capacity;
        do {
            new_capacity *= 2; // Double the capacity
            // Handle potential overflow if capacity becomes huge, though unlikely with size_t
            if (new_capacity < rb->capacity) { // Check for overflow
                 new_capacity = needed_capacity; // Fallback if doubling overflows
                 if (new_capacity < rb->size || new_capacity < data_len) { // Check needed_capacity didn't overflow
                      fprintf(stderr, "Error: RingBuffer capacity overflow during resize calculation.\n");
                      errno = EOVERFLOW;
                      return 0; // Cannot resize sufficiently
                 }
                 // If needed_capacity is valid but doubling overflowed, use needed_capacity if it's larger than current capacity
                 if (new_capacity <= rb->capacity) {
                     fprintf(stderr, "Error: RingBuffer cannot grow large enough.\n");
                     errno = ENOMEM;
                     return 0;
                 }
                 break; // Use the calculated needed_capacity (if valid & larger)
            }
        } while (new_capacity < needed_capacity);


        // Attempt to resize
        if (ring_buffer_resize(rb, new_capacity) != 0) {
            // Resizing failed (e.g., out of memory)
            // Try to write whatever fits in the *current* available space
            size_t bytes_to_write = available_space;
             if (bytes_to_write == 0) return 0; // Buffer is completely full, resize failed

            // Proceed with writing partial data (existing logic)
             size_t available_space_to_end = rb->capacity - rb->head;

            if (bytes_to_write <= available_space_to_end) {
                memcpy(rb->buffer + rb->head, data, bytes_to_write);
                rb->head = (rb->head + bytes_to_write) % rb->capacity; // Use modulo for safety
            } else {
                memcpy(rb->buffer + rb->head, data, available_space_to_end);
                memcpy(rb->buffer, data + available_space_to_end, bytes_to_write - available_space_to_end);
                rb->head = bytes_to_write - available_space_to_end;
            }
            rb->size += bytes_to_write;
            fprintf(stderr, "Warning: RingBuffer resize failed, wrote partial data (%zu bytes)\n", bytes_to_write);
            return bytes_to_write; // Return the amount actually written
        }
         // Resizing succeeded, available_space is now updated implicitly by the change in rb->capacity
         // The write logic below will now handle the full data_len
    }

     // --- Original write logic (now guaranteed to have enough space) ---
    size_t bytes_to_write = data_len; // We know we have space now

    size_t available_space_to_end = rb->capacity - rb->head;

    if (bytes_to_write <= available_space_to_end) {
        memcpy(rb->buffer + rb->head, data, bytes_to_write);
        rb->head += bytes_to_write;
        if (rb->head == rb->capacity) { // Wrap around if head reaches end
            rb->head = 0;
        }
    } else { // Data wraps around
        memcpy(rb->buffer + rb->head, data, available_space_to_end);
        memcpy(rb->buffer, data + available_space_to_end, bytes_to_write - available_space_to_end);
        rb->head = bytes_to_write - available_space_to_end;
    }
    rb->size += bytes_to_write;
    return bytes_to_write;
}

void ring_buffer_free(RingBuffer *rb)
{
    if (!rb)
        return;
    free(rb->buffer);
    free(rb);
}

void ring_buffer_reset(RingBuffer *rb)
{
    if (!rb)
        return;
    rb->head = 0;
    rb->tail = 0;
    rb->size = 0;
}

size_t ring_buffer_get_size(const RingBuffer *rb)
{
    return rb ? rb->size : 0;
}

size_t ring_buffer_get_capacity(const RingBuffer *rb)
{
    return rb ? rb->capacity : 0;
}
int ring_buffer_is_empty(const RingBuffer *rb)
{
    return rb ? rb->size == 0 : 1;
}

int ring_buffer_is_full(const RingBuffer *rb)
{
    return rb ? rb->size == rb->capacity : 0;
}


size_t ring_buffer_read(RingBuffer *rb, char *dest, size_t dest_len)
{
    if (!rb || !dest || dest_len == 0 || ring_buffer_is_empty(rb))
        return 0;

    size_t bytes_to_read = dest_len;
    if (bytes_to_read > rb->size)
    {
        bytes_to_read = rb->size; // Don't read more than what's in buffer
    }
    if (bytes_to_read == 0)
        return 0;

    size_t available_data_to_end = rb->capacity - rb->tail;

    if (bytes_to_read <= available_data_to_end)
    {
        memcpy(dest, rb->buffer + rb->tail, bytes_to_read);
        rb->tail += bytes_to_read;
        if (rb->tail == rb->capacity)
        { // Wrap around if tail reaches end
            rb->tail = 0;
        }
    }
    else
    { // Data wraps around
        memcpy(dest, rb->buffer + rb->tail, available_data_to_end);
        memcpy(dest + available_data_to_end, rb->buffer, bytes_to_read - available_data_to_end);
        rb->tail = bytes_to_read - available_data_to_end;
    }
    rb->size -= bytes_to_read;
    return bytes_to_read;
}

size_t ring_buffer_peek(const RingBuffer *rb, char *dest, size_t dest_len)
{
    if (!rb || !dest || dest_len == 0 || ring_buffer_is_empty(rb))
        return 0;

    size_t bytes_to_peek = dest_len;
    if (bytes_to_peek > rb->size)
    {
        bytes_to_peek = rb->size;
    }
    if (bytes_to_peek == 0)
        return 0;

    size_t available_data_to_end = rb->capacity - rb->tail;

    if (bytes_to_peek <= available_data_to_end)
    {
        memcpy(dest, rb->buffer + rb->tail, bytes_to_peek);
        // Do not advance tail for peek operation
    }
    else
    {
        memcpy(dest, rb->buffer + rb->tail, available_data_to_end);
        memcpy(dest + available_data_to_end, rb->buffer, bytes_to_peek - available_data_to_end);
    }

    return bytes_to_peek; // Return how many bytes we peeked
}

char *ring_buffer_readline(RingBuffer *rb, char *line_buffer, size_t line_buffer_size)
{
    // --- Argument Validation ---
    if (!rb || !line_buffer || line_buffer_size == 0) {
        return NULL; // Invalid arguments
    }
    line_buffer[0] = '\0'; // Ensure buffer is empty initially or on early return

    size_t bytes_in_rb = ring_buffer_get_size(rb);
    if (bytes_in_rb == 0) {
        return NULL; // Empty buffer, no line possible
    }

    // --- Efficiently Find Newline Offset ---
    size_t newline_offset = (size_t)-1; // Sentinel for not found
    for (size_t i = 0; i < bytes_in_rb; ++i) {
        // Calculate index without modifying rb->tail yet
        size_t current_idx = (rb->tail + i) % rb->capacity;
        if (rb->buffer[current_idx] == '\n') {
            newline_offset = i;
            break;
        }
    }

    // If no newline was found in the available data
    if (newline_offset == (size_t)-1) {
        return NULL;
    }

    // --- Determine Actual Line Length and Ending Type ---
    // newline_offset is the index relative to tail where '\n' is.
    size_t actual_line_len = newline_offset; // Length initially excludes \n
    int cr_found = 0;
    size_t ending_len = 1; // Bytes to consume for line ending (\n)

    // Check if the character *before* '\n' is '\r'
    if (newline_offset > 0) {
        size_t prev_idx = (rb->tail + newline_offset - 1) % rb->capacity;
        if (rb->buffer[prev_idx] == '\r') {
            cr_found = 1;
            actual_line_len = newline_offset - 1; // Actual line content excludes \r too
            ending_len = 2; // Line ending is \r\n
        }
    }

    // --- Read Actual Line Content (up to buffer size limit) ---
    size_t len_to_copy = actual_line_len;
    if (len_to_copy >= line_buffer_size) {
        // fprintf(stderr, "Warning: ring_buffer_readline truncated line (len %zu) to fit buffer (size %zu)\n",
        //         actual_line_len, line_buffer_size);
        len_to_copy = line_buffer_size - 1; // Leave space for null terminator
    }

    // Use ring_buffer_read to get the actual line content.
    // This advances rb->tail past the content.
    size_t bytes_read = ring_buffer_read(rb, line_buffer, len_to_copy);
    if (bytes_read != len_to_copy) {
        // This indicates an internal error in ring_buffer_read or the logic here.
        fprintf(stderr, "Error: ring_buffer_readline failed to read expected line content (%zu != %zu).\n", bytes_read, len_to_copy);
        // State is potentially inconsistent. Returning NULL is safest.
        // We might have partially read data, leaving the buffer state difficult to recover.
        return NULL;
    }
    line_buffer[len_to_copy] = '\0'; // Null-terminate the string in the destination buffer.

    // --- Consume Remaining Part of Line (if truncated) and the Line Ending ---

    // Bytes of the *actual line* that were not copied because the buffer was too small
    size_t remaining_line_bytes_to_discard = actual_line_len - len_to_copy;

    // Total bytes remaining in the buffer that belong to this line (truncated part + ending)
    size_t total_bytes_to_consume = remaining_line_bytes_to_discard + ending_len;

    // Consume these bytes efficiently
    if (total_bytes_to_consume > 0) {
        // Optimization: Can we just advance head/tail directly if possible?
        // ring_buffer_read internally advances tail and decreases size.
        // We can just read into a small discard buffer or potentially optimize within ring_buffer_read itself if needed.
        char discard_buffer[16]; // Small temporary buffer
        size_t consumed_count = 0;
        while (consumed_count < total_bytes_to_consume) {
            size_t amount_to_consume_now = total_bytes_to_consume - consumed_count;
            if (amount_to_consume_now > sizeof(discard_buffer)) {
                amount_to_consume_now = sizeof(discard_buffer);
            }
            size_t just_consumed = ring_buffer_read(rb, discard_buffer, amount_to_consume_now);

            // If ring_buffer_read returns 0 before consuming all expected bytes,
            // it means the buffer became empty unexpectedly (error).
            if (just_consumed == 0 && (consumed_count < total_bytes_to_consume)) {
                 fprintf(stderr, "Error: ring_buffer_readline buffer became empty while consuming line remainder/ending (consumed %zu/%zu).\n",
                         consumed_count, total_bytes_to_consume);
                 // The buffer state is now likely corrupt relative to expectations.
                 return NULL; // Indicate error
            }
            consumed_count += just_consumed;
        }
         // Optional check: Verify total consumed amount
         if (consumed_count != total_bytes_to_consume) {
              fprintf(stderr, "Warning: ring_buffer_readline consumed %zu bytes, expected to consume %zu for remainder+ending.\n",
                     consumed_count, total_bytes_to_consume);
             // Proceed, but log this potential issue.
         }
    }

    return line_buffer; // Success
}