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
#include "metrics.h"
#include "server_config.h"
#include "event_loop.h"
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
 * Read a base-10 integer from a /proc/sys file. Returns 0 on success and
 * leaves *out unchanged on any failure so callers can distinguish
 * "unsupported on this host" from a real value.
 */
static int read_sysctl_long(const char *path, long *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long value = 0;
    int ok = (fscanf(fp, "%ld", &value) == 1);
    fclose(fp);
    if (!ok)
        return -1;
    *out = value;
    return 0;
}

/*
 * Parse a sysctl vector of the form "min default max" (tcp_rmem/tcp_wmem)
 * into the three longs. Returns 0 on success, -1 when unreadable/malformed.
 */
static int read_sysctl_vector(const char *path, long out[3])
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long a = 0, b = 0, c = 0;
    int ok = (fscanf(fp, "%ld %ld %ld", &a, &b, &c) == 3);
    fclose(fp);
    if (!ok)
        return -1;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    return 0;
}

/*
 * Phase 5: raise the soft RLIMIT_NOFILE up to the hard limit and re-read it.
 * The server depends on a soft limit above its connection capacity plus the
 * reserved headroom; a lower limit means the application cap cannot be
 * honored, so this is reported and (by the caller) treated as fatal.
 *
 * `required` is the minimum soft limit the configuration needs. On return,
 * `rl` holds the effective (post-setrlimit) limit, and the return value is
 * 0 when the effective soft limit meets `required`, -1 otherwise.
 */
static unsigned long rlim_value(rlim_t value)
{
    return value == RLIM_INFINITY ? ULONG_MAX : (unsigned long)value;
}

static int enforce_nofile_limit(unsigned long required, int el_threads,
                                struct rlimit *rl)
{
    if (getrlimit(RLIMIT_NOFILE, rl) != 0) {
        perror("getrlimit RLIMIT_NOFILE");
        return -1;
    }

    unsigned long before = rlim_value(rl->rlim_cur);

    /* Try to raise the soft limit to the hard limit. EPERM is not fatal: the
     * hard limit may be lower than we want, which the check below catches. */
    if (rl->rlim_cur != rl->rlim_max) {
        struct rlimit raised = *rl;
        raised.rlim_cur = raised.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &raised) != 0) {
            /* Not an error by itself; we re-read and report the effective
             * value so an unprivileged host still fails clearly on the check. */
            fprintf(stderr,
                    "NOTE: could not raise soft RLIMIT_NOFILE to hard limit: "
                    "%s\n", strerror(errno));
        }
        if (getrlimit(RLIMIT_NOFILE, rl) != 0) {
            perror("getrlimit RLIMIT_NOFILE");
            return -1;
        }
    }

    unsigned long effective = rlim_value(rl->rlim_cur);

    printf("  FD limit: soft=%lu (was %lu), hard=%lu, required>=%lu\n",
           effective, before, rlim_value(rl->rlim_max), required);

    if (effective < required) {
        fprintf(stderr,
                "FATAL: effective soft RLIMIT_NOFILE=%lu is below the required "
                "%lu (MAX_ACTIVE_CONNECTIONS=%ld + headroom=%d + %d/loop * %d "
                "loop(s)); raise the limit (ulimit -n / LimitNOFILE) or lower "
                "%s\n",
                effective, required,
                (long)((MAX_ACTIVE_CONNECTIONS)),
                REQUIRED_NOFILE_HEADROOM, REQUIRED_NOFILE_PER_LOOP,
                el_threads,
                ENV_MAX_CONNECTIONS);
        return -1;
    }
    return 0;
}

/*
 * Phase 5: read the kernel accept-queue cap and TCP buffer limits, compute the
 * effective backlog, and report them. The server never mutates sysctls; it
 * verifies and reports so a benchmark row is interpretable.
 */
static void report_host_limits(void)
{
    long somaxconn = -1;
    if (read_sysctl_long(PROC_SOMAXCONN, &somaxconn) == 0) {
        long effective_backlog = (somaxconn < BACKLOG) ? somaxconn : BACKLOG;
        printf("  somaxconn: %ld, configured backlog=%d, effective backlog=%ld%s\n",
               somaxconn, BACKLOG, effective_backlog,
               (effective_backlog < BACKLOG) ? " (clamped by somaxconn)" : "");
        if (effective_backlog < BACKLOG) {
            fprintf(stderr,
                    "WARNING: BACKLOG=%d cannot be honored: somaxconn=%ld; "
                    "effective backlog is %ld. Raise net.core.somaxconn above "
                    "%d to use the configured backlog.\n",
                    BACKLOG, somaxconn, effective_backlog, BACKLOG);
        }
    } else {
        printf("  somaxconn: unavailable (%s); effective backlog=%d\n",
               PROC_SOMAXCONN, BACKLOG);
    }

    long rmem[3] = {0, 0, 0};
    long wmem[3] = {0, 0, 0};
    int have_rmem = read_sysctl_vector(PROC_TCP_RMEM, rmem) == 0;
    int have_wmem = read_sysctl_vector(PROC_TCP_WMEM, wmem) == 0;
    if (have_rmem)
        printf("  tcp_rmem: min=%ld default=%ld max=%ld\n",
               rmem[0], rmem[1], rmem[2]);
    if (have_wmem)
        printf("  tcp_wmem: min=%ld default=%ld max=%ld\n",
               wmem[0], wmem[1], wmem[2]);
}

/*
 * Phase 5: validate the compile-time/runtime configuration. Rejects
 * non-positive limits and contradictory combinations, naming the offending
 * value so an operator can fix it directly. Returns 0 when valid, -1 on the
 * first violation.
 */
static int validate_configuration(int el_threads, long capacity)
{
    if (PORT <= 0 || PORT > 65535) {
        fprintf(stderr, "FATAL: PORT=%d is out of range 1..65535\n", PORT);
        return -1;
    }
    if (BACKLOG <= 0) {
        fprintf(stderr, "FATAL: BACKLOG=%d must be positive\n", BACKLOG);
        return -1;
    }
    if (MAX_ACTIVE_CONNECTIONS <= 0) {
        fprintf(stderr,
                "FATAL: MAX_ACTIVE_CONNECTIONS=%d must be positive\n",
                MAX_ACTIVE_CONNECTIONS);
        return -1;
    }
    if (MAX_INPUT_BUFFER_BYTES <= 0) {
        fprintf(stderr,
                "FATAL: MAX_INPUT_BUFFER_BYTES=%d must be positive\n",
                MAX_INPUT_BUFFER_BYTES);
        return -1;
    }
    if (MAX_PIPELINE_DEPTH <= 0) {
        fprintf(stderr, "FATAL: MAX_PIPELINE_DEPTH=%d must be positive\n",
                MAX_PIPELINE_DEPTH);
        return -1;
    }
    if (HEADER_READ_TIMEOUT_SEC <= 0 || IDLE_TIMEOUT_SEC <= 0 ||
        WRITE_TIMEOUT_SEC <= 0) {
        fprintf(stderr,
                "FATAL: timeouts must be positive (header=%d, idle=%d, "
                "write=%d)\n",
                HEADER_READ_TIMEOUT_SEC, IDLE_TIMEOUT_SEC,
                WRITE_TIMEOUT_SEC);
        return -1;
    }
    if (el_threads < 1) {
        fprintf(stderr, "FATAL: resolved event-loop count %d must be >= 1\n",
                el_threads);
        return -1;
    }
    if (capacity < 1) {
        fprintf(stderr, "FATAL: effective connection capacity %ld must be >= 1\n",
                capacity);
        return -1;
    }
    if (capacity > EL_MAX_CONNECTION_TABLE) {
        fprintf(stderr,
                "FATAL: effective connection capacity %ld exceeds "
                "EL_MAX_CONNECTION_TABLE=%d\n",
                capacity, EL_MAX_CONNECTION_TABLE);
        return -1;
    }
    return 0;
}

/*
 * Phase 5: optional CPU affinity. Unset by default; enabled only from a
 * measured result. The applied mask is printed so a benchmark row records it.
 * Returns 0 when unset or successfully applied, -1 on a malformed request or
 * a failed sched_setaffinity (which is fatal: silently ignoring a requested
 * pin would invalidate the measurement).
 */
static int apply_cpu_affinity(void)
{
    const char *env = getenv(ENV_CPU_SET);
    if (!env || !*env)
        return 0;

    cpu_set_t set;
    CPU_ZERO(&set);
    const char *cursor = env;
    while (*cursor) {
        char *end = NULL;
        errno = 0;
        long first = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || first < 0) {
            fprintf(stderr, "FATAL: %s=%s is not a valid CPU list\n",
                    ENV_CPU_SET, env);
            return -1;
        }
        long last = first;
        cursor = end;
        if (*cursor == '-') {
            cursor++;
            errno = 0;
            last = strtol(cursor, &end, 10);
            if (errno != 0 || end == cursor || last < first) {
                fprintf(stderr, "FATAL: %s=%s is not a valid CPU range\n",
                        ENV_CPU_SET, env);
                return -1;
            }
            cursor = end;
        }
        for (long cpu = first; cpu <= last; cpu++) {
            if (cpu >= CPU_SETSIZE) {
                fprintf(stderr, "FATAL: %s=%s names CPU %ld beyond CPU_SETSIZE\n",
                        ENV_CPU_SET, env, cpu);
                return -1;
            }
            CPU_SET((int)cpu, &set);
        }
        if (*cursor == ',') {
            cursor++;
        } else if (*cursor != '\0') {
            fprintf(stderr, "FATAL: %s=%s has an unexpected separator\n",
                    ENV_CPU_SET, env);
            return -1;
        }
    }

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "FATAL: sched_setaffinity(%s=%s) failed: %s\n",
                ENV_CPU_SET, env, strerror(errno));
        return -1;
    }

    cpu_set_t applied;
    CPU_ZERO(&applied);
    if (sched_getaffinity(0, sizeof(applied), &applied) == 0) {
        char mask[CPU_SETSIZE * 4];
        size_t off = 0;
        int first = 1;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET(cpu, &applied)) {
                int n = snprintf(mask + off, sizeof(mask) - off, "%s%d",
                                 first ? "" : ",", cpu);
                if (n < 0 || (size_t)n >= sizeof(mask) - off)
                    break;
                off += (size_t)n;
                first = 0;
            }
        }
        printf("  CPU affinity: %s -> {%s}\n",
               ENV_CPU_SET, first ? "" : mask);
    }
    return 0;
}

/*
 * Derive the effective connection capacity at startup.
 *
 * The value is the minimum of:
 *   - the operator-configured maximum (HTTP_SERVER_MAX_CONNECTIONS, or
 *     MAX_ACTIVE_CONNECTIONS when unset),
 *   - the descriptor-derived capacity (soft RLIMIT_NOFILE minus the reserved
 *     headroom for the listener, epoll, metrics, logs, and shutdown, and the
 *     per-loop descriptors), and
 *   - the hard EL_MAX_CONNECTION_TABLE memory bound.
 *
 * Clamps down safely and prints every input so a benchmark row can be
 * reproduced. Fails (returns a non-positive value) when the effective capacity
 * would be zero, which the caller treats as a fatal startup error.
 */
static long derive_effective_capacity(const struct rlimit *rl, int el_threads)
{
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

    long reserved = (long)REQUIRED_NOFILE_HEADROOM +
                    (long)REQUIRED_NOFILE_PER_LOOP * el_threads;
    long descriptor_cap = LONG_MAX;
    if (rl->rlim_cur != RLIM_INFINITY) {
        descriptor_cap = (long)rl->rlim_cur - reserved;
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

    printf("  Connection capacity: operator_max=%ld, descriptor_cap=%ld, "
           "effective=%ld (limited by %s)\n",
           operator_max,
           descriptor_cap == LONG_MAX ? -1L : descriptor_cap,
           effective,
           limited_by);

    if (effective < 1) {
        fprintf(stderr,
                "FATAL: effective connection capacity is %ld; raise "
                "RLIMIT_NOFILE above %ld or lower %s\n",
                effective, reserved, ENV_MAX_CONNECTIONS);
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

    /* Print configuration and run the Phase 5 startup preflight. */
    print_server_config();

    /* Resolve how many event loops to run: explicit override or one per core.
     * Needed before the descriptor preflight so per-loop fds are reserved. */
    int el_threads = event_loop_thread_count();

    /* Raise the soft descriptor limit as far as the hard limit allows and
     * fail clearly when the effective limit cannot cover the configured
     * capacity plus reserved headroom. */
    unsigned long required_nofile = (unsigned long)MAX_ACTIVE_CONNECTIONS +
                                    (unsigned long)REQUIRED_NOFILE_HEADROOM +
                                    (unsigned long)REQUIRED_NOFILE_PER_LOOP *
                                        (unsigned long)el_threads;
    struct rlimit rl;
    if (enforce_nofile_limit(required_nofile, el_threads, &rl) != 0) {
        return EXIT_FAILURE;
    }

    long capacity = derive_effective_capacity(&rl, el_threads);
    if (capacity < 1) {
        return EXIT_FAILURE;
    }
    metrics_set_connection_capacity(capacity);

    report_host_limits();

    if (validate_configuration(el_threads, capacity) != 0) {
        return EXIT_FAILURE;
    }

    if (apply_cpu_affinity() != 0) {
        return EXIT_FAILURE;
    }

    /* Cache static responses before accepting any clients. */
    if (initialize_static_responses() != 0) {
        return EXIT_FAILURE;
    }

    /* Bind and listen; SO_REUSEPORT is required when several loops share the
     * port, and deliberately omitted for the single-loop control. */
    server_socket = create_server_socket(el_threads > 1);
    if (server_socket < 0) {
        return EXIT_FAILURE;
    }
    printf("Server listening on port %d...\n", PORT);

    printf("Dispatch model: epoll event loop (%d loop%s, capacity=%ld).\n",
           el_threads, el_threads == 1 ? "" : "s", capacity);
    event_loop_run(server_socket, &server_running, capacity, el_threads);

    printf("\nShutting down server gracefully...\n");
    close(server_socket);
    metrics_reporter_stop();

    return 0;
}
