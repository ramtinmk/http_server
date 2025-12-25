/*
 * Simple HTTP Server for Educational Purposes
 *
 * This implementation demonstrates basic socket programming
 * and HTTP protocol handling. Not recommended for production use.
 */
#include "http_server.h"
#include "thread_pool.h"

void sigchld_handler(int sig);
// static void parse_request_line(char *line, HTTPRequest *req);
// static void parse_header_line(char *line, HTTPRequest *req);



// Helper functions

void drive_state_machine(Connection *conn) {
    switch (conn->state) {
        case STATE_READING_REQUEST:
            // Read from socket into conn->in_buffer.
            // If read returns EAGAIN, return.
            // If we have a full line, parse it.
            // If parsed OK, conn->state = STATE_PROCESSING;

            break;

        case STATE_PROCESSING:
            // Open file, prepare headers.
            // Write headers to conn->out_buffer.
            // conn->state = STATE_SENDING_HEADER;
            break;

        case STATE_SENDING_HEADER:
            // Write conn->out_buffer to socket.
            // If EAGAIN, return.
            // If buffer empty, conn->state = STATE_SENDING_BODY;
            break;
            
        case STATE_SENDING_BODY:
            // Read chunk from file -> Write to socket.
            // If EAGAIN, save file_offset and return.
            // If done, conn->state = STATE_READING_REQUEST (Keep-alive)
            break;
    }
}




int validate_request(HTTPRequest *req);
// --- Thread Pool Configuration ---


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







/**
 * @brief Main handler for a client connection.
 *
 * Manages the connection lifecycle, reads data into a ring buffer using select()
 * for timeouts, and calls process_single_request in a loop to handle pipelined
 * requests until the connection is closed or an error occurs.
 *
 * @param client_socket The client's socket file descriptor.
 */





