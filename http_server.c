/*
 * Simple HTTP Server for Educational Purposes
 *
 * This implementation demonstrates basic socket programming
 * and HTTP protocol handling. Not recommended for production use.
 */

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>    // For open() flags like O_RDONLY
#include <sys/time.h> // Required for struct timeval in select
#include <errno.h>    // For errno
//changes:
#include <sys/stat.h>  // For struct stat, fstat()
#include <unistd.h>    // For fstat() and file operations

void sigchld_handler(int sig);
static void parse_request_line(char *line, HTTPRequest *req);
static void parse_header_line(char *line, HTTPRequest *req);

ThreadPool *create_thread_pool(int pool_size);
void destroy_thread_pool(ThreadPool *pool);
void add_task_to_queue(ThreadPool *pool, int client_socket);
Task *get_task_from_queue(ThreadPool *pool); // Or Task* get_task_from_queue(...) depending on task structure return
void *worker_thread_function(void *arg);

// Error responses
const char *BAD_REQUEST_400 = ERROR_TEMPLATE("400 Bad Request", "Malformed request syntax");
const char *NOT_FOUND_404 = ERROR_TEMPLATE("404 Not Found", "The requested resource was not found");
const char *NOT_IMPLEMENTED_501 = ERROR_TEMPLATE("501 Not Implemented", "HTTP method not supported");

// Supported methods
const char *SUPPORTED_METHODS[] = {"GET", "HEAD"};
const int SUPPORTED_METHOD_COUNT = 2;

// Helper functions
int validate_request(HTTPRequest *req);

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

    strncpy(req->headers[req->header_count][0], line, 255);
    req->headers[req->header_count][0][255] = '\0';
    strncpy(req->headers[req->header_count][1], value, 255);
    req->headers[req->header_count][1][255] = '\0';
    req->header_count++;
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
void handle_client(int client_socket)
{
    RingBuffer *request_rb = ring_buffer_create(BUFFER_SIZE);
    if (!request_rb)
    {
        perror("Failed to create request ring buffer");
        return; // Or handle error as appropriate
    }
    RingBuffer *response_rb = ring_buffer_create(BUFFER_SIZE); // You might use this later for more complex responses
    if (!response_rb)
    {
        perror("Failed to create response ring buffer");
        ring_buffer_free(request_rb);
        return; // Or handle error as appropriate
    }
    ssize_t bytes_read;
    int keep_alive_connection = 1; // Start with keep-alive enabled
    char line_buffer[BUFFER_SIZE]; // Temporary buffer for reading lines

    while (keep_alive_connection)
    {
        HTTPRequest request = {0};
        request.keep_alive = 1; // Default to keep-alive unless client requests close

        char temp_buffer[BUFFER_SIZE]; // Temporary buffer for reading from socket

        // --- Implement Timeout using select() ---
        struct timeval tv;
        tv.tv_sec = IDLE_TIMEOUT_SEC; // Set timeout seconds
        tv.tv_usec = 0;               // Set timeout microseconds (0 for simplicity)

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_socket, &readfds); // Watch client socket for readability

        int select_result = select(client_socket + 1, &readfds, NULL, NULL, &tv); // Wait for socket to be ready to read, or timeout

        if (select_result == -1)
        {
            perror("select"); // Log select error
            fprintf(stderr, "handle_client: select() error on socket %d, closing connection\n", client_socket);
            break; // Select error, close connection
        }
        else if (select_result == 0)
        {
            // Timeout occurred
            fprintf(stderr, "handle_client: Timeout on socket %d after %d seconds of inactivity. Closing connection.\n", client_socket, IDLE_TIMEOUT_SEC);
            break; // Timeout, close connection
        }
        else
        {
            // Socket is ready to read (select_result > 0) - proceed with read()
            bytes_read = read(client_socket, temp_buffer, BUFFER_SIZE);
            fprintf(stderr, "handle_client: read() returned %zd bytes from socket %d\n", bytes_read, client_socket);
            if (bytes_read > 0)
            {
                ring_buffer_write(request_rb, temp_buffer, bytes_read);
                fprintf(stderr, "handle_client: Wrote %zd bytes to request ring buffer\n", bytes_read);
            }
            if (bytes_read <= 0)
            {
                if (bytes_read < 0)
                    perror("read");
                fprintf(stderr, "handle_client: read() returned <= 0, connection closed or error on socket %d, exiting keep-alive loop\n", client_socket);
                break;
            }
            // Parse request line from ring buffer
            char *request_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
            if (!request_line)
            {
                send_error_response(client_socket, BAD_REQUEST_400);
                break; // Bad request, close keep-alive loop
            }
            parse_request_line(request_line, &request);

            // Validate method
            if (!method_is_supported(request.method))
            {
                send_error_response(client_socket, NOT_IMPLEMENTED_501);
                break; // Unsupported method, close keep-alive loop
            }

            // Parse headers from ring buffer
            while (1)
            {
                char *header_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
                if (!header_line)
                {
                    // No more complete lines in buffer, or error
                    break;
                }
                if (header_line[0] == '\0')
                {
                    // Empty line indicates end of headers
                    break;
                }
                parse_header_line(header_line, &request);
            }

            // Check for Connection: close header
            for (int i = 0; i < request.header_count; i++)
            {
                if (strcasecmp(request.headers[i][0], "Connection") == 0)
                {
                    if (strcasecmp(request.headers[i][1], "close") == 0)
                    {
                        request.keep_alive = 0;    // Client explicitly requested to close
                        keep_alive_connection = 0; // Signal to close keep-alive loop after this response
                    }
                    break; // Found Connection header, no need to check further
                }
            }

            char filepath[1024];
            long file_size = 0;
            int file_fd = -1; // Initialize to -1 for error checking

            if (strcmp(request.path, "/home") == 0)
            {
                snprintf(filepath, sizeof(filepath), "home.html");
            }
            else if (strcmp(request.path, "/hello") == 0)
            {
                snprintf(filepath, sizeof(filepath), "hello.html");
            }
            else if (strcmp(request.path, "/") == 0)
            {
                snprintf(filepath, sizeof(filepath), "home.html");
            }
            else
            {
                send_error_response(client_socket, NOT_FOUND_404);
                keep_alive_connection = request.keep_alive; // Honor client's keep-alive preference for error responses
                continue;                                   // Continue to next request in keep-alive, or close if client requested
            }

            file_fd = open(filepath, O_RDONLY); // Use open() here
            if (file_fd == -1)
            {
                perror("open");
                send_error_response(client_socket, NOT_FOUND_404);
                keep_alive_connection = request.keep_alive;
                continue;
            }

            // Get file size (using file descriptor now if needed, or reuse your existing file_size logic)
            struct stat file_stat;
            if (fstat(file_fd, &file_stat) == -1)
            { // Get file stats from fd
                perror("fstat");
                close(file_fd); // Close fd on error
                send_error_response(client_socket, NOT_FOUND_404);
                keep_alive_connection = request.keep_alive;
                continue;
            }
            file_size = file_stat.st_size;

            char response_headers[BUFFER_SIZE];
            snprintf(response_headers, BUFFER_SIZE,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Length: %ld\r\n"
                     "\r\n",
                     file_size);

            if (write(client_socket, response_headers, strlen(response_headers)) < 0)
            {
                perror("write headers");
                close(file_fd);
                break;
            }

            char buffer[4096];
            ssize_t bytes_read;
            lseek(file_fd, 0, SEEK_SET); // Ensure we read from the start
            
            while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
                if (write(client_socket, buffer, bytes_read) < 0) {
                    perror("write");
                    break;
                }
            }
            
            close(file_fd);  // Close file descriptor

            if (!request.keep_alive)
            {
                keep_alive_connection = 0;
            }
        }
        printf("Keep-alive connection closed for client socket %d\n", client_socket);
        ring_buffer_free(request_rb);
        ring_buffer_free(response_rb);
    }
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

// --- Ring Buffer Function Implementations ---

RingBuffer *ring_buffer_create(size_t capacity)
{
    RingBuffer *rb = malloc(sizeof(RingBuffer));
    if (!rb)
        return NULL;
    rb->buffer = malloc(capacity);
    if (!rb->buffer)
    {
        free(rb);
        return NULL;
    }
    rb->capacity = capacity;
    ring_buffer_reset(rb); // Initialize head, tail, size
    return rb;
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

size_t ring_buffer_write(RingBuffer *rb, const char *data, size_t data_len)
{
    if (!rb || !data || data_len == 0 || ring_buffer_is_full(rb))
        return 0;

    size_t bytes_to_write = data_len;
    if (bytes_to_write > rb->capacity - rb->size)
    { // Ensure we don't overflow
        bytes_to_write = rb->capacity - rb->size;
    }
    if (bytes_to_write == 0)
        return 0; // Buffer is full

    size_t available_space_to_end = rb->capacity - rb->head;

    if (bytes_to_write <= available_space_to_end)
    {
        memcpy(rb->buffer + rb->head, data, bytes_to_write);
        rb->head += bytes_to_write;
        if (rb->head == rb->capacity)
        { // Wrap around if head reaches end
            rb->head = 0;
        }
    }
    else
    { // Data wraps around
        memcpy(rb->buffer + rb->head, data, available_space_to_end);
        memcpy(rb->buffer, data + available_space_to_end, bytes_to_write - available_space_to_end);
        rb->head = bytes_to_write - available_space_to_end;
    }
    rb->size += bytes_to_write;
    return bytes_to_write;
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
    size_t original_tail = rb->tail; // Keep original tail for peek operation

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
    size_t bytes_in_rb = ring_buffer_get_size(rb);
    if (bytes_in_rb == 0)
        return NULL;

    size_t bytes_to_newline = 0;
    size_t peeked_bytes;
    char peek_buffer[BUFFER_SIZE]; // Temp buffer for peeking

    while (bytes_to_newline < bytes_in_rb)
    {
        peeked_bytes = ring_buffer_peek(rb, peek_buffer, bytes_to_newline + 1);
        if (peeked_bytes <= bytes_to_newline)
            break; // Should not happen, but safety check
        if (peek_buffer[bytes_to_newline] == '\n')
            break; // Found newline
        bytes_to_newline++;
    }

    if (bytes_to_newline >= bytes_in_rb)
        return NULL; // No newline found in buffer yet

    size_t line_len = bytes_to_newline; // Length excluding newline
    if (line_len >= line_buffer_size)
        line_len = line_buffer_size - 1;         // Prevent overflow
    ring_buffer_read(rb, line_buffer, line_len); // Actually read the line
    line_buffer[line_len] = '\0';

    // Consume the newline character(s) - assuming \r\n
    char newline_chars[2];
    ring_buffer_read(rb, newline_chars, 2); // Try to read \r\n
    if (newline_chars[0] != '\r' || newline_chars[1] != '\n')
    {
        // Handle error if newline sequence is not as expected, or just consume what we can.
    }

    return line_buffer;
}