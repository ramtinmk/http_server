#define _GNU_SOURCE   /* accept4, SOCK_CLOEXEC */
/*
 * Simple HTTP Server — main entry point
 *
 * Responsibilities:
 *   - Server socket setup (FD_CLOEXEC, SO_REUSEADDR, listen).
 *   - Single-threaded accept loop with connection-level admission control.
 *   - File-descriptor limit validation at startup.
 *   - Graceful shutdown on SIGINT / SIGTERM.
 */
#include "http_server.h"
#include "thread_pool.h"
#include "metrics.h"
#include "server_config.h"
#if USE_EVENT_LOOP
#include "event_loop.h"
#endif
#include <signal.h>
#include <errno.h>
#include <sys/resource.h>

static volatile sig_atomic_t server_running = 1;

static void shutdown_signal_handler(int sig)
{
    (void)sig;
    server_running = 0;
}

/* --------------------------------------------------------------------------
 * Startup diagnostics
 * -------------------------------------------------------------------------- */

/*
 * Print every configured limit to stdout so that benchmark runs can be
 * reproduced from server logs alone.
 */
static void print_server_config(void)
{
    printf("=== Server Configuration ===\n");
    printf("  PORT                  : %d\n",  PORT);
    printf("  BACKLOG               : %d\n",  BACKLOG);
    printf("  THREAD_POOL_SIZE      : %d\n",  THREAD_POOL_SIZE);
    printf("  MAX_QUEUED_TASKS      : %d\n",  MAX_QUEUED_TASKS);
    printf("  MAX_ACTIVE_CONNECTIONS: %d\n",  MAX_ACTIVE_CONNECTIONS);
    printf("  MAX_KEEPALIVE_REQUESTS: %d\n",  MAX_KEEPALIVE_REQUESTS);
    printf("  MAX_INPUT_BUFFER_BYTES: %d\n",  MAX_INPUT_BUFFER_BYTES);
    printf("  MAX_PIPELINE_DEPTH    : %d\n",  MAX_PIPELINE_DEPTH);
    printf("  HEADER_READ_TIMEOUT   : %d s\n",HEADER_READ_TIMEOUT_SEC);
    printf("  IDLE_TIMEOUT          : %d s\n",IDLE_TIMEOUT_SEC);
    printf("  WRITE_TIMEOUT         : %d s\n",WRITE_TIMEOUT_SEC);
    printf("============================\n");
}

/*
 * Verify that the process's soft file-descriptor limit is large enough to
 * handle MAX_ACTIVE_CONNECTIONS client sockets plus operational headroom.
 * Prints a warning (not a fatal error) if the limit is too low, so that an
 * operator can raise it without breaking the server for small workloads.
 */
static void check_nofile_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("getrlimit RLIMIT_NOFILE");
        return;
    }

    long required = (long)MAX_ACTIVE_CONNECTIONS + (long)REQUIRED_NOFILE_HEADROOM;

    printf("  FD limit: soft=%lu, hard=%lu (server requires >= %ld)\n",
           (unsigned long)rl.rlim_cur,
           (unsigned long)rl.rlim_max,
           required);

    if (rl.rlim_cur != RLIM_INFINITY && (long)rl.rlim_cur < required) {
        fprintf(stderr,
                "WARNING: RLIMIT_NOFILE soft limit (%lu) is below the required "
                "%ld. Run `ulimit -n %ld` before starting the server, or reduce "
                "MAX_ACTIVE_CONNECTIONS.\n",
                (unsigned long)rl.rlim_cur, required, required);
    }
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(void)
{
    int server_socket;

    /* Optional structured metrics snapshots for the benchmark harness. */
    const char *metrics_path = getenv("HTTP_SERVER_METRICS_FILE");
    if (metrics_path && *metrics_path) {
        metrics_reporter_start(metrics_path, 250);
    }

    /* Graceful shutdown: don't use SA_RESTART so accept() unblocks on signal. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Print configuration and validate file-descriptor limits. */
    print_server_config();
    check_nofile_limit();

    /* Cache static responses before accepting any clients. */
    if (initialize_static_responses() != 0) {
        return EXIT_FAILURE;
    }

    /* Bind and listen. */
    server_socket = create_server_socket();
    printf("Server listening on port %d...\n", PORT);

#if USE_EVENT_LOOP
    /* ------------------------------------------------------------------ */
    /* Phase 3: single-threaded nonblocking epoll event loop.              */
    /* The thread pool is not used; all admitted sockets are owned by the  */
    /* event loop.                                                          */
    /* ------------------------------------------------------------------ */
    printf("Dispatch model: epoll event loop (Phase 3).\n");
    event_loop_run(server_socket, &server_running);

    printf("\nShutting down server gracefully...\n");
    close(server_socket);
    metrics_reporter_stop();

#else
    /* ------------------------------------------------------------------ */
    /* Phase 2: blocking thread-pool model.                                */
    /* ------------------------------------------------------------------ */
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    ThreadPool *thread_pool = create_thread_pool(THREAD_POOL_SIZE);
    if (thread_pool == NULL) {
        fprintf(stderr, "Failed to create thread pool\n");
        return EXIT_FAILURE;
    }
    printf("Thread pool initialized with %d threads (queue max: %d).\n",
           THREAD_POOL_SIZE, MAX_QUEUED_TASKS);
    printf("Dispatch model: blocking thread pool (Phase 2).\n");

    /* ---------- Accept loop -------------------------------------------- */
    while (server_running) {
#ifdef SOCK_CLOEXEC
        client_socket = accept4(server_socket,
                                (struct sockaddr *)&client_addr,
                                &addr_size,
                                SOCK_CLOEXEC);
#else
        client_socket = accept(server_socket,
                               (struct sockaddr *)&client_addr,
                               &addr_size);
        if (client_socket >= 0) {
            int fl = fcntl(client_socket, F_GETFD);
            if (fl >= 0) {
                fcntl(client_socket, F_SETFD, fl | FD_CLOEXEC);
            }
        }
#endif
        if (client_socket == -1) {
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }

        metrics_connection_accepted();

        if (access_log) {
            printf("Client connected: %s:%d\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
        }

        if (metrics_active_connection_count() >= (long)MAX_ACTIVE_CONNECTIONS) {
            metrics_admission_rejected();
            close(client_socket);
            continue;
        }

        metrics_active_connection_inc();

        if (add_task_to_queue(thread_pool, client_socket) != 0) {
            metrics_active_connection_dec();
            close(client_socket);
        }
    }
    /* ---------- End of accept loop ------------------------------------- */

    printf("\nShutting down server gracefully...\n");
    close(server_socket);

    destroy_thread_pool(thread_pool);
    metrics_reporter_stop();
    printf("Thread pool destroyed.\n");
#endif /* USE_EVENT_LOOP */

    return 0;
}
