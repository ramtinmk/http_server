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
#include "config.h"
#include "log.h"
#include "privilege.h"
#include "sd_notify.h"
#include "tls.h"
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

static volatile sig_atomic_t server_running = 1;

/* Resolved allocator arena cap for the startup report; empty when the concept
 * does not apply (non-glibc) or was preset by the operator. */
static char arena_cap_display[64];

/* --------------------------------------------------------------------------
 * Allocator setup
 * -------------------------------------------------------------------------- */

/*
 * Bound glibc's per-thread malloc arenas so VmSize does not scale with the
 * event-loop count. Each arena reserves virtual address space (not RSS); the
 * server's allocator traffic is per-connection buffer churn on the loop
 * threads. Called before any thread exists so every thread inherits the cap.
 * A pre-set MALLOC_ARENA_MAX wins; otherwise HTTP_SERVER_MALLOC_ARENA_MAX
 * overrides the compiled default. Returns 0 on success (including non-glibc
 * and preset hosts), -1 on a malformed override.
 */
static int configure_allocator(void)
{
#if defined(__GLIBC__) && defined(M_ARENA_MAX)
    const char *preset = getenv("MALLOC_ARENA_MAX");
    if (preset && *preset) {
        snprintf(arena_cap_display, sizeof(arena_cap_display),
                 "preset by MALLOC_ARENA_MAX=%s", preset);
        return 0;
    }

    long value = MALLOC_ARENA_MAX_DEFAULT;
    const char *env = getenv(ENV_MALLOC_ARENA_MAX);
    if (env && *env) {
        errno = 0;
        char *end = NULL;
        long parsed = strtol(env, &end, 10);
        if (errno != 0 || end == env || *end != '\0' || parsed < 1) {
            fprintf(stderr, "FATAL: %s=%s is not a positive integer\n",
                    ENV_MALLOC_ARENA_MAX, env);
            return -1;
        }
        value = parsed;
    }

    if (mallopt(M_ARENA_MAX, (int)value) == 0) {
        fprintf(stderr,
                "WARNING: mallopt(M_ARENA_MAX, %ld) failed; leaving the "
                "allocator default\n", value);
        return 0;
    }
    snprintf(arena_cap_display, sizeof(arena_cap_display), "%ld", value);
#else
    arena_cap_display[0] = '\0';
#endif
    return 0;
}

static void shutdown_signal_handler(int sig)
{
    (void)sig;
    server_running = 0;
    /* Fail readiness before the drain so load balancers stop sending work.
     * Atomic store only; this runs in signal context. */
    metrics_set_ready(0);
}

/* --------------------------------------------------------------------------
 * Startup diagnostics
 * -------------------------------------------------------------------------- */

/*
 * Print every configured limit to stdout so that benchmark runs can be
 * reproduced from server logs alone. Also records the effective configuration
 * as a structured log line when a log target is active.
 */
static void print_server_config(const ServerConfig *cfg)
{
    printf("=== Server Configuration ===\n");
    printf("  PORT                  : %d\n",  cfg->port);
    printf("  BACKLOG               : %d\n",  cfg->backlog);
    printf("  EL_ACCEPT_BATCH_SIZE  : %d\n",  EL_ACCEPT_BATCH_SIZE);
    printf("  MAX_ACTIVE_CONNECTIONS: %ld\n", cfg->max_connections);
    printf("  MAX_KEEPALIVE_REQUESTS: %d\n",  cfg->max_keepalive_requests);
    printf("  MAX_INPUT_BUFFER_BYTES: %d\n",  cfg->max_input_buffer_bytes);
    printf("  MAX_PIPELINE_DEPTH    : %d\n",  MAX_PIPELINE_DEPTH);
    printf("  HEADER_READ_TIMEOUT   : %d s\n",cfg->header_read_timeout_sec);
    printf("  IDLE_TIMEOUT          : %d s\n",cfg->idle_timeout_sec);
    printf("  WRITE_TIMEOUT         : %d s\n",cfg->write_timeout_sec);
    printf("  SHUTDOWN_DRAIN_TIMEOUT: %d s\n",cfg->shutdown_drain_timeout_sec);
    printf("  EL_THREAD_COUNT       : %d%s\n",  EL_THREAD_COUNT,
           EL_THREAD_COUNT == 0 ? " (auto: online cores)" : "");
    printf("  EL_MAX_THREADS        : %d\n",  EL_MAX_THREADS);
    printf("  EL_MAX_CONNECTION_TABLE: %d\n", EL_MAX_CONNECTION_TABLE);
    printf("  LOG_LEVEL             : %s\n",  log_level_name(cfg->log_level));
    printf("  LOG_FILE              : %s\n",
           cfg->log_file[0] ? cfg->log_file : "(stderr)");
    printf("  ACCESS_LOG            : %s\n",  cfg->access_log ? "on" : "off");
    printf("  DOCUMENT_ROOT         : %s\n",  cfg->document_root);
    printf("  INDEX_FILES           : %s\n",  cfg->index_files);
    printf("  MIME_TYPES            : %s\n",
           cfg->mime_types_file[0] ? cfg->mime_types_file : "(builtin)");
    printf("  HIDDEN_FILES          : %s\n",
           cfg->hidden_files_allowed ? "allowed" : "denied");
    printf("  SYMLINKS              : %s\n",
           cfg->symlinks_allowed ? "allowed" : "denied");
    printf("  CACHE_BUDGET_BYTES    : %ld\n", cfg->cache_budget_bytes);
    printf("  TLS                   : %s\n", cfg->tls_enabled ? "on" : "off");
    if (cfg->tls_enabled) {
        printf("  TLS_PORT              : %d\n", cfg->tls_port);
        printf("  TLS_CERT_FILE         : %s\n", cfg->tls_cert_file);
        printf("  TLS_KEY_FILE          : %s\n", cfg->tls_key_file);
    }
    printf("  CONFIG_FILE           : %s\n",
           cfg->config_path[0] ? cfg->config_path : "(none)");
    printf("  MALLOC_ARENA_MAX (cap) : %s\n",
           arena_cap_display[0] ? arena_cap_display : "not applicable");
    printf("  RUN_USER              : %s\n",
           cfg->run_user[0] ? cfg->run_user : "(unchanged)");
    printf("  RUN_GROUP             : %s\n",
           cfg->run_group[0] ? cfg->run_group : "(user's primary)");
    printf("  OBSERVABILITY         : %s\n",
           cfg->observability_enabled ? "on" : "off");
    if (cfg->observability_enabled) {
        printf("  METRICS_PATH          : %s\n", cfg->metrics_path);
        printf("  HEALTH_PATH           : %s\n", cfg->health_path);
        printf("  READINESS_PATH        : %s\n", cfg->readiness_path);
    }
    printf("  SYSLOG                : %s\n", cfg->syslog_enabled ? "on" : "off");
    printf("============================\n");

    log_msg(LOG_LEVEL_INFO,
            "effective_config port=%d backlog=%d max_connections=%ld "
            "max_keepalive_requests=%d max_input_buffer_bytes=%d "
            "header_read_timeout=%d idle_timeout=%d write_timeout=%d "
            "shutdown_drain_timeout=%d log_level=%s access_log=%d "
            "log_file=%s tls=%d tls_port=%d config_file=%s "
            "run_user=%s run_group=%s observability=%d syslog=%d "
            "metrics_path=%s health_path=%s readiness_path=%s",
            cfg->port, cfg->backlog, cfg->max_connections,
            cfg->max_keepalive_requests, cfg->max_input_buffer_bytes,
            cfg->header_read_timeout_sec, cfg->idle_timeout_sec,
            cfg->write_timeout_sec, cfg->shutdown_drain_timeout_sec,
            log_level_name(cfg->log_level), cfg->access_log,
            cfg->log_file[0] ? cfg->log_file : "(stderr)",
            cfg->tls_enabled, cfg->tls_port,
            cfg->config_path[0] ? cfg->config_path : "(none)",
            cfg->run_user[0] ? cfg->run_user : "(none)",
            cfg->run_group[0] ? cfg->run_group : "(none)",
            cfg->observability_enabled, cfg->syslog_enabled,
            cfg->metrics_path, cfg->health_path, cfg->readiness_path);
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
                                long operator_max, struct rlimit *rl)
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
                operator_max,
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
static int validate_configuration(const ServerConfig *cfg, int el_threads,
                                  long capacity)
{
    if (cfg->port <= 0 || cfg->port > 65535) {
        fprintf(stderr, "FATAL: port=%d is out of range 1..65535\n", cfg->port);
        return -1;
    }
    if (cfg->backlog <= 0) {
        fprintf(stderr, "FATAL: backlog=%d must be positive\n", cfg->backlog);
        return -1;
    }
    if (cfg->max_connections <= 0) {
        fprintf(stderr, "FATAL: max_connections=%ld must be positive\n",
                cfg->max_connections);
        return -1;
    }
    if (cfg->max_input_buffer_bytes <= 0) {
        fprintf(stderr, "FATAL: max_input_buffer_bytes=%d must be positive\n",
                cfg->max_input_buffer_bytes);
        return -1;
    }
    if (cfg->header_read_timeout_sec <= 0 || cfg->idle_timeout_sec <= 0 ||
        cfg->write_timeout_sec <= 0 || cfg->shutdown_drain_timeout_sec <= 0) {
        fprintf(stderr,
                "FATAL: timeouts must be positive (header=%d, idle=%d, "
                "write=%d, drain=%d)\n",
                cfg->header_read_timeout_sec, cfg->idle_timeout_sec,
                cfg->write_timeout_sec, cfg->shutdown_drain_timeout_sec);
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
static long derive_effective_capacity(const ServerConfig *cfg,
                                      const struct rlimit *rl, int el_threads)
{
    long operator_max = cfg->max_connections;
    if (operator_max <= 0) {
        fprintf(stderr, "FATAL: max_connections=%ld must be positive\n",
                operator_max);
        return -1;
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

static void reload_signal_handler(int sig)
{
    (void)sig;
    log_request_reopen();
    tls_request_reload();
    config_request_reload();
}

int main(int argc, char **argv)
{
    ServerConfig cfg;
    config_defaults(&cfg);
    if (config_load(&cfg, argc, argv) != 0)
        return EXIT_FAILURE;
    if (cfg.help_requested) {
        config_print_usage();
        return EXIT_SUCCESS;
    }
    /* Remember the sources so SIGHUP can re-parse the reloadable subset. */
    config_set_reload_args(argc, argv);
    metrics_set_start_time((long long)time(NULL));

    /* Bound allocator address-space reservations before any thread exists. */
    if (configure_allocator() != 0)
        return EXIT_FAILURE;

    /* Structured logging: open the target and start the writer thread. */
    if (log_init(&cfg) != 0)
        return EXIT_FAILURE;

    /* Optional structured metrics snapshots for the benchmark harness. */
    const char *metrics_path = getenv("HTTP_SERVER_METRICS_FILE");
    if (metrics_path && *metrics_path) {
        metrics_set_cache_sampler(http_server_cache_stats);
        metrics_reporter_start(metrics_path, 250);
    }

    /* Graceful shutdown: don't use SA_RESTART so accept() unblocks on signal.
     * SIGHUP reopens the log file. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = reload_signal_handler;
    sigaction(SIGHUP, &sa, NULL);

    /* Print the effective configuration and record it in the startup log. */
    print_server_config(&cfg);

    /* Resolve the configured run identity now so a typo fails fast, naming the
     * key, before any listener is bound. The irreversible drop itself happens
     * after every listener exists (inside event_loop_run). */
    if (privilege_validate(&cfg) != 0) {
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Resolve how many event loops to run: explicit override or one per core.
     * Needed before the descriptor preflight so per-loop fds are reserved. */
    int el_threads = event_loop_thread_count();

    /* Raise the soft descriptor limit as far as the hard limit allows and
     * fail clearly when the effective limit cannot cover the configured
     * capacity plus reserved headroom. */
    unsigned long required_nofile = (unsigned long)cfg.max_connections +
                                    (unsigned long)REQUIRED_NOFILE_HEADROOM +
                                    (unsigned long)REQUIRED_NOFILE_PER_LOOP *
                                        (unsigned long)el_threads;
    struct rlimit rl;
    if (enforce_nofile_limit(required_nofile, el_threads, cfg.max_connections,
                             &rl) != 0) {
        log_shutdown();
        return EXIT_FAILURE;
    }

    long capacity = derive_effective_capacity(&cfg, &rl, el_threads);
    if (capacity < 1) {
        log_shutdown();
        return EXIT_FAILURE;
    }
    metrics_set_connection_capacity(capacity);

    report_host_limits();

    if (validate_configuration(&cfg, el_threads, capacity) != 0) {
        log_shutdown();
        return EXIT_FAILURE;
    }

    if (apply_cpu_affinity() != 0) {
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Load the certificate and build the shared TLS context (no-op when TLS is
     * disabled) before any listener is bound. */
    if (tls_init(&cfg) != 0) {
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Cache static responses and configure the document root before accepting
     * any clients. */
    if (initialize_static_responses(&cfg) != 0) {
        tls_shutdown();
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Bind and listen; SO_REUSEPORT is required when several loops share the
     * port, and deliberately omitted for the single-loop control. */
    int server_socket = create_server_socket(&cfg, el_threads > 1);
    if (server_socket < 0) {
        tls_shutdown();
        log_shutdown();
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d...\n", cfg.port);
    printf("Dispatch model: epoll event loop (%d loop%s, capacity=%ld).\n",
           el_threads, el_threads == 1 ? "" : "s", capacity);

    metrics_set_ready(1);
    log_msg(LOG_LEVEL_INFO, "server ready port=%d tls=%d loops=%d capacity=%ld",
            cfg.port, cfg.tls_enabled, el_threads, capacity);
    if (sd_notify_ready("serving") != 0)
        fprintf(stderr, "WARNING: sd_notify READY failed\n");

    int run_status = event_loop_run(server_socket, &server_running, &cfg,
                                    capacity, el_threads);

    printf("\nShutting down server gracefully...\n");
    log_msg(LOG_LEVEL_INFO, "shutdown complete status=%d dropped_logs=%lld",
            run_status, log_dropped_total());
    if (sd_notify_stopping("drain complete") != 0)
        fprintf(stderr, "WARNING: sd_notify STOPPING failed\n");

    close(server_socket);
    metrics_reporter_stop();
    shutdown_static_responses();
    tls_shutdown();
    log_shutdown();

    return run_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
