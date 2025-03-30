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
    // Use the potentially modified create function with an initial capacity
    RingBuffer *request_rb = ring_buffer_create(BUFFER_SIZE); // Or INITIAL_RING_BUFFER_CAPACITY
    if (!request_rb)
    {
        perror("Failed to create request ring buffer");
        // Consider sending 500 Internal Server Error before closing
        return;
    }
    // Keep response_rb creation simple unless it also needs dynamic resizing later
    RingBuffer *response_rb = ring_buffer_create(BUFFER_SIZE);
    if (!response_rb)
    {
        perror("Failed to create response ring buffer");
        ring_buffer_free(request_rb);
         // Consider sending 500 Internal Server Error before closing
        return;
    }

    ssize_t bytes_read;
    int keep_alive_connection = 1; // Start with keep-alive enabled
    char line_buffer[BUFFER_SIZE]; // Temporary buffer for reading lines

    // *** The rest of handle_client remains the same ***
    // ... (select loop, read, ring_buffer_write, parsing, sending response) ...
    // The key change is that ring_buffer_write inside the loop will now handle
    // resizing automatically if temp_buffer contains more data than fits.

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
            if (errno == EINTR) continue; // Interrupted by signal, retry select
            perror("select"); // Log select error
            fprintf(stderr, "handle_client: select() error on socket %d, closing connection\n", client_socket);
            break; // Select error, close connection
        }
        else if (select_result == 0)
        {
            // Timeout occurred
            // Only log if buffer is empty, otherwise maybe we are just waiting for more data mid-request?
             // If keep-alive, maybe just close if idle too long, even if buffer not empty?
             // Let's stick to simple timeout means close for now.
             if (ring_buffer_is_empty(request_rb)) {
                 fprintf(stderr, "handle_client: Timeout on socket %d after %d seconds of inactivity. Closing connection.\n", client_socket, IDLE_TIMEOUT_SEC);
             } else {
                 fprintf(stderr, "handle_client: Timeout on socket %d after %d seconds while waiting for more data. Closing connection.\n", client_socket, IDLE_TIMEOUT_SEC);
             }
            break; // Timeout, close connection
        }
        else // select_result > 0 -> socket is ready
        {
            bytes_read = read(client_socket, temp_buffer, sizeof(temp_buffer));
            fprintf(stderr, "handle_client: read() returned %zd bytes from socket %d\n", bytes_read, client_socket);

            if (bytes_read < 0) {
                 if (errno == EINTR) continue; // Interrupted by signal, retry read
                 if (errno == EAGAIN || errno == EWOULDBLOCK) {
                     // This shouldn't happen after select indicated readability, but handle defensively
                     fprintf(stderr, "handle_client: read() indicated EWOULDBLOCK on supposedly ready socket %d. Continuing.\n", client_socket);
                     continue;
                 }
                 perror("read");
                 fprintf(stderr, "handle_client: read() error on socket %d, closing connection\n", client_socket);
                 break; // Read error, close connection
            }
             if (bytes_read == 0) {
                 // Client closed connection gracefully
                 fprintf(stderr, "handle_client: Client on socket %d closed connection (read returned 0).\n", client_socket);
                 keep_alive_connection = 0; // Ensure loop terminates after this potential final request processing
                 // Don't break immediately, process any data remaining in the buffer first!
            }
            else { // bytes_read > 0
                // Write data to ring buffer - THIS WILL NOW RESIZE IF NEEDED
                size_t written = ring_buffer_write(request_rb, temp_buffer, bytes_read);
                fprintf(stderr, "handle_client: Wrote %zu bytes to request ring buffer (capacity %zu, size %zu)\n",
                        written, ring_buffer_get_capacity(request_rb), ring_buffer_get_size(request_rb));
                if (written < (size_t)bytes_read) {
                    // This indicates write failed even after potential resize attempt (e.g., hit max limit or OOM)
                    fprintf(stderr, "Error: Failed to write all read data to ring buffer on socket %d. Closing connection.\n", client_socket);
                    // Consider sending 500 Internal Server Error
                     send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Server buffer capacity exceeded"));
                    break; // Cannot proceed reliably
                }
            }

            // --- Try to process a complete request from the buffer ---
            // This part needs careful looping if keep-alive is active and buffer might contain partial next request
            int processed_request = 0; // Flag to see if we successfully processed one request in this iteration

            // Loop as long as we can successfully parse a full request from the buffer
            while(1) {
                 // Peek to see if we have a potential end-of-headers marker (\r\n\r\n)
                 // This is tricky with ring buffer peek. A simpler approach is to just try parsing.
                 // We rely on ring_buffer_readline returning NULL if a full line isn't available.

                 // Save state in case parsing fails mid-request
                 size_t initial_rb_size = ring_buffer_get_size(request_rb);
                 size_t initial_rb_tail = request_rb->tail;
                 // Need to also save head? No, read/readline modifies tail. Resetting tail is enough.

                 // Try to read request line
                 char *request_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
                 if (!request_line) {
                     // Not enough data for a complete request line yet.
                     // If client closed connection (bytes_read == 0), and we still don't have a line, it's an incomplete request.
                     if (bytes_read == 0 && initial_rb_size > 0) {
                          fprintf(stderr, "handle_client: Connection closed by client with incomplete request data in buffer on socket %d.\n", client_socket);
                          // No valid request to process, force close.
                          keep_alive_connection = 0;
                     } else if (bytes_read == 0 && initial_rb_size == 0) {
                         // Connection closed and buffer empty, just exit loop cleanly.
                         keep_alive_connection = 0;
                     }
                     // Otherwise (bytes_read > 0), break inner loop and wait for more data from select()
                     break; // Break the while(1) parsing loop, go back to select()
                 }

                 // Reset request struct for parsing
                 memset(&request, 0, sizeof(request));
                 request.keep_alive = 1; // Assume keep-alive for HTTP/1.1

                 parse_request_line(request_line, &request);

                 // Basic validation of parsed line (e.g., check if method/path look reasonable)
                 if (strlen(request.method) == 0 || strlen(request.path) == 0) {
                     fprintf(stderr, "handle_client: Failed to parse request line: '%s'. Sending 400.\n", line_buffer); // Use line_buffer as request_line points to it
                     send_error_response(client_socket, BAD_REQUEST_400);
                     keep_alive_connection = 0; // Bad request, close connection
                     // Consume the rest of the buffer? Risky. Best to close.
                      // Need to break outer loop too
                      goto end_client_handling; // Jump out of both loops
                 }


                 // Validate method
                 if (!method_is_supported(request.method)) {
                     fprintf(stderr, "handle_client: Unsupported method '%s'. Sending 501.\n", request.method);
                     send_error_response(client_socket, NOT_IMPLEMENTED_501);
                     keep_alive_connection = request.keep_alive; // Honor keep-alive for error
                      // Consume rest of this request's headers if possible? Or just close?
                      // Let's keep it simple: Close if method not supported seems safer.
                      keep_alive_connection = 0;
                      goto end_client_handling; // Jump out
                 }

                 // Parse headers
                 request.header_count = 0; // Reset header count
                 int header_parse_complete = 0;
                 while (1) {
                     char *header_line = ring_buffer_readline(request_rb, line_buffer, sizeof(line_buffer));
                     if (!header_line) {
                         // Not enough data for the next header line or the final empty line.
                          // If client closed, this is an incomplete request.
                         if (bytes_read == 0) {
                             fprintf(stderr, "handle_client: Connection closed by client mid-headers on socket %d.\n", client_socket);
                              send_error_response(client_socket, BAD_REQUEST_400); // Send 400 for incomplete request
                              keep_alive_connection = 0;
                              goto end_client_handling; // Jump out
                         }
                         // Otherwise, need more data. We consumed the request line, but need to put it back? No, readline consumes.
                         // We need to rollback the ring buffer state.
                         // Reset tail to where it was before trying to parse this request.
                          request_rb->tail = initial_rb_tail;
                          request_rb->size = initial_rb_size;
                         // Now break inner loop to wait for more data
                         goto wait_for_more_data; // Use goto to break out of header loop and request loop
                     }
                     if (header_line[0] == '\0') {
                         // Empty line indicates end of headers
                         header_parse_complete = 1;
                         break; // Exit header parsing loop
                     }
                     parse_header_line(header_line, &request); // Assumes parse_header_line handles max headers etc.
                 }

                 if (!header_parse_complete) {
                      // This should technically be caught by !header_line case above, but defensive check.
                      fprintf(stderr, "handle_client: Internal error - Header loop exited without finding end of headers on socket %d.\n", client_socket);
                      keep_alive_connection = 0;
                      goto end_client_handling;
                 }


                 // Check for Connection: close header *after* parsing all headers
                 for (int i = 0; i < request.header_count; i++) {
                     if (strcasecmp(request.headers[i][0], "Connection") == 0) {
                         if (strcasecmp(request.headers[i][1], "close") == 0) {
                             request.keep_alive = 0;
                         }
                         // Could also handle "keep-alive" explicitly if needed, but HTTP/1.1 default is keep-alive
                         break;
                     }
                 }
                 // Update outer loop control based on parsed request
                 keep_alive_connection = request.keep_alive;


                 // --- Process the valid, fully parsed request ---
                 processed_request = 1; // Mark that we are processing a request now

                 // TODO: Handle potential request body based on Content-Length or Transfer-Encoding
                 // For this simple server, assume GET/HEAD have no body we need to read/discard

                 char filepath[1024];
                 long file_size = 0;
                 int file_fd = -1;

                 // Determine filepath (same logic as before)
                 if (strcmp(request.path, "/home") == 0 || strcmp(request.path, "/") == 0) {
                     snprintf(filepath, sizeof(filepath), "home.html");
                 } else if (strcmp(request.path, "/hello") == 0) {
                     snprintf(filepath, sizeof(filepath), "hello.html");
                 } else {
                     fprintf(stderr, "handle_client: Path '%s' not found. Sending 404.\n", request.path);
                     send_error_response(client_socket, NOT_FOUND_404);
                      // Honor keep-alive preference for this error
                     if (!request.keep_alive) break; // Break inner loop if close requested
                     else continue; // Continue inner loop to check for pipelined request
                 }

                 file_fd = open(filepath, O_RDONLY);
                 if (file_fd == -1) {
                     perror("open");
                     fprintf(stderr, "handle_client: File '%s' not found or error opening. Sending 404.\n", filepath);
                     send_error_response(client_socket, NOT_FOUND_404);
                      if (!request.keep_alive) break;
                     else continue;
                 }

                 struct stat file_stat;
                 if (fstat(file_fd, &file_stat) == -1) {
                     perror("fstat");
                     close(file_fd);
                     fprintf(stderr, "handle_client: Error getting file stats for '%s'. Sending 500.\n", filepath);
                     send_error_response(client_socket, ERROR_TEMPLATE("500 Internal Server Error", "Error accessing file details"));
                      keep_alive_connection = 0; // Internal error, probably close
                      break; // Break inner loop
                 }
                 file_size = file_stat.st_size;

                 // Send Response Headers
                 char response_headers[BUFFER_SIZE];
                 snprintf(response_headers, sizeof(response_headers),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html\r\n"
                          "Connection: %s\r\n" // Use keep-alive or close based on request
                          "Content-Length: %ld\r\n"
                          "\r\n",
                          request.keep_alive ? "keep-alive" : "close",
                          file_size);

                 if (write(client_socket, response_headers, strlen(response_headers)) < 0) {
                     perror("write headers");
                     close(file_fd);
                      keep_alive_connection = 0; // Write error, close
                     break; // Break inner loop
                 }

                 // Send Response Body (only for GET)
                 if (strcmp(request.method, "GET") == 0) {
                    off_t offset = 0;
                    // Use sendfile for efficiency
                    ssize_t sent_bytes = sendfile(client_socket, file_fd, &offset, file_size);
                    if (sent_bytes == -1) {
                        perror("sendfile");
                        // Don't close file_fd yet, it's closed below
                        keep_alive_connection = 0; // Error during send, close
                         close(file_fd);
                        break; // Break inner loop
                    } else if (sent_bytes != file_size) {
                        fprintf(stderr, "Warning: sendfile sent %zd bytes, expected %ld for %s\n", sent_bytes, file_size, filepath);
                        // Connection likely closed by client mid-transfer. Treat as error for keep-alive.
                        keep_alive_connection = 0;
                         close(file_fd);
                        break; // Break inner loop
                    }
                 } else if (strcmp(request.method, "HEAD") == 0) {
                     // HEAD request: Headers already sent, no body needed.
                 }

                 close(file_fd); // Close file descriptor for this request

                 // If client requested close, break the inner processing loop
                 if (!request.keep_alive) {
                      break; // Break inner while(1)
                 }

                 // Otherwise, loop back to see if buffer contains another complete request (pipelining)

            } // End while(1) parsing loop

        wait_for_more_data:; // Label for goto when more data is needed

        } // End else (select_result > 0)

        // If keep_alive_connection became false during processing, break the outer loop
        if (!keep_alive_connection) {
            break; // Break the outer while(keep_alive_connection)
        }

        // If bytes_read was 0 (client closed) and we processed everything, break outer loop
        if (bytes_read == 0 && ring_buffer_is_empty(request_rb)) {
             break; // Break outer loop
        }


    } // End while(keep_alive_connection) loop

end_client_handling:; // Label for jumping out on critical errors

    fprintf(stderr, "handle_client: Finished handling client on socket %d. Final keep-alive state: %d. Buffer size: %zu\n",
             client_socket, keep_alive_connection, ring_buffer_get_size(request_rb));

    // Cleanup: Free ring buffers
    ring_buffer_free(request_rb);
    ring_buffer_free(response_rb);
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