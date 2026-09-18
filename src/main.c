/*
 * Simple HTTP Server
 */
#include "http_server.h"
#include "thread_pool.h"
#include "metrics.h"
#include <signal.h>
#include <errno.h>

static volatile sig_atomic_t server_running = 1;

static void shutdown_signal_handler(int sig)
{
    (void)sig;
    server_running = 0;
}

int main(void)
{
    int server_socket, client_socket;
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    // Access logging is disabled during throughput measurements.
    const char *access_log_env = getenv("HTTP_SERVER_ACCESS_LOG");
    int access_log = !(access_log_env && strcmp(access_log_env, "0") == 0);

    // Optional structured metrics snapshots for the benchmark harness.
    const char *metrics_path = getenv("HTTP_SERVER_METRICS_FILE");
    if (metrics_path && *metrics_path) {
        metrics_reporter_start(metrics_path, 250);
    }

    // Set up graceful shutdown handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Do not use SA_RESTART so accept() unblocks on signal
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Cache supported static responses before accepting any clients.
    if (initialize_static_responses() != 0) {
        return EXIT_FAILURE;
    }

    // Initialize Thread Pool
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

    while (server_running)
    {
        // Accept incoming connection
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
        if (client_socket == -1)
        {
            if (errno == EINTR)
            {
                // Interrupted by shutdown signal
                break;
            }
            perror("accept");
            continue;
        }

        metrics_connection_accepted();
        if (access_log) {
            printf("Client connected: %s:%d\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
        }

        add_task_to_queue(thread_pool, client_socket);
    }

    printf("\nShutting down server gracefully...\n");
    close(server_socket);

    // Destroy Thread Pool before exiting
    destroy_thread_pool(thread_pool);
    metrics_reporter_stop();
    printf("Thread pool destroyed.\n");

    return 0;
}





