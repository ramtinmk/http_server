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
#include <limits.h>
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
    printf("  EL_THREAD_COUNT       : %d%s\n",  EL_THREAD_COUNT,
           EL_THREAD_COUNT == 0 ? " (auto: online cores)" : "");
    printf("  EL_MAX_THREADS        : %d\n",  EL_MAX_THREADS);
    printf("  EL_MAX_CONNECTION_TABLE: %d\n", EL_MAX_CONNECTION_TABLE);
    printf("============================\n");
}

/*
 * Derive the effective connection capacity at startup.
 *
 * The value is the minimum of:
 *   - the operator-configured maximum (HTTP_SERVER_MAX_CONNECTIONS, or
 *     MAX_ACTIVE_CONNECTIONS when unset),
 *   - the descriptor-derived capacity (soft RLIMIT_NOFILE minus the reserved
 *     headroom for the listener, epoll, metrics, logs, and shutdown), and
 *   - the hard EL_MAX_CONNECTION_TABLE memory bound.
 *
 * Clamps down safely and prints every input so a benchmark row can be
 * reproduced. Fails (returns a non-positive value) when the effective capacity
 * would be zero, which the caller treats as a fatal startup error.
 */
static long derive_effective_capacity(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("getrlimit RLIMIT_NOFILE");
        return -1;
    }

    long operator_max = MAX_ACTIVE_CONNECTIONS;
    const char *env = getenv(ENV_MAX_CONNECTIONS);
    if (env && *env) {
        errno = 0;
        char *end = NULL;
        long parsed = strtol(env, &end, 10);
        if (errno != 0 || end == env || *end != '\0' || parsed <= 0) {
            fprintf(stderr,
                    "FATAL: %s=%s is not a positive integer\n",
                    ENV_MAX_CONNECTIONS, env);
            return -1;
        }
        operator_max = parsed;
    }

    long descriptor_cap = LONG_MAX;
    if (rl.rlim_cur != RLIM_INFINITY) {
        descriptor_cap = (long)rl.rlim_cur - (long)REQUIRED_NOFILE_HEADROOM;
    }

    long effective = operator_max;
    const char *limited_by = "operator max";
    if (descriptor_cap < effective) {
        effective   = descriptor_cap;
        limited_by  = "RLIMIT_NOFILE";
    }
    if (EL_MAX_CONNECTION_TABLE < effective) {
        effective  = EL_MAX_CONNECTION_TABLE;
        limited_by = "EL_MAX_CONNECTION_TABLE";
    }

    printf("  FD limit: soft=%lu, hard=%lu, reserved=%d\n",
           (unsigned long)rl.rlim_cur,
           (unsigned long)rl.rlim_max,
           REQUIRED_NOFILE_HEADROOM);
    printf("  Connection capacity: operator_max=%ld, descriptor_cap=%ld, "
           "effective=%ld (limited by %s)\n",
           operator_max,
           descriptor_cap == LONG_MAX ? -1L : descriptor_cap,
           effective,
           limited_by);

    if (effective < 1) {
        fprintf(stderr,
                "FATAL: effective connection capacity is %ld; raise "
                "RLIMIT_NOFILE above %d or lower %s\n",
                effective, REQUIRED_NOFILE_HEADROOM, ENV_MAX_CONNECTIONS);
        return -1;
    }
    if (operator_max > effective) {
        fprintf(stderr,
                "WARNING: requested %s=%ld exceeds effective capacity %ld; "
                "clamping to %ld\n",
                ENV_MAX_CONNECTIONS, operator_max, effective, effective);
    }
    return effective;
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

    /* Print configuration and derive the effective connection capacity. */
    print_server_config();
    long capacity = derive_effective_capacity();
    if (capacity < 1) {
        return EXIT_FAILURE;
    }
    metrics_set_connection_capacity(capacity);

    /* Cache static responses before accepting any clients. */
    if (initialize_static_responses() != 0) {
        return EXIT_FAILURE;
    }

    /* Resolve how many event loops to run: explicit override or one per core. */
#if USE_EVENT_LOOP
    int el_threads = event_loop_thread_count();
#else
    int el_threads = 0;
#endif

    /* Bind and listen; SO_REUSEPORT is required when several loops share the
     * port, and deliberately omitted for the single-loop control. */
    server_socket = create_server_socket(el_threads > 1);
    printf("Server listening on port %d...\n", PORT);

#if USE_EVENT_LOOP
    /* ------------------------------------------------------------------ */
    /* Phase 3/4: nonblocking epoll event loops.                           */
    /* One loop runs per online CPU core (or the EL_THREAD_COUNT override). */
    /* Values > 1 add SO_REUSEPORT listeners so the kernel distributes new  */
    /* connections; each loop exclusively owns the sockets it accepts.      */
    /* ------------------------------------------------------------------ */
    printf("Dispatch model: epoll event loop (Phase 4, %d loop%s, "
           "capacity=%ld).\n",
           el_threads, el_threads == 1 ? "" : "s", capacity);
    event_loop_run(server_socket, &server_running, capacity, el_threads);

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
