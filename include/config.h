#ifndef CONFIG_H
#define CONFIG_H

/*
 * Single validated configuration source.
 *
 * Precedence (low to high): compile-time defaults from server_config.h, then a
 * key=value configuration file (HTTP_SERVER_CONFIG or --config), then
 * environment variables, then command-line flags. Every value is range-checked
 * and an invalid key or value is fatal and names the offending key.
 *
 * The compile-time event-loop count (EL_THREAD_COUNT) is deliberately NOT part
 * of this surface: it remains a compile-time override by project convention.
 */

#include <stddef.h>
#include "server_config.h"   /* RUN_USER_MAX */

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} LogLevel;

typedef struct ServerConfig {
    int      port;
    int      backlog;
    long     max_connections;            /* operator cap (admission) */
    int      max_keepalive_requests;
    int      max_input_buffer_bytes;
    int      header_read_timeout_sec;
    int      idle_timeout_sec;
    int      write_timeout_sec;
    int      shutdown_drain_timeout_sec;

    /* Logging */
    LogLevel log_level;
    int      access_log;                 /* 0/1; access lines emitted when 1 */
    char     log_file[4096];             /* empty: log to stderr */

    /* Phase 2: static file serving */
    char     document_root[4096];        /* root directory for resolved paths */
    char     index_files[1024];          /* comma-separated directory indexes */
    char     mime_types_file[4096];      /* optional mime.types file */
    int      hidden_files_allowed;       /* 0/1; serve dotfiles when 1 */
    int      symlinks_allowed;           /* 0/1; follow in-root symlinks when 1 */
    long     cache_budget_bytes;         /* representation-cache byte budget */

    /* Phase 3: TLS termination */
    int      tls_enabled;                /* 0/1; bind a TLS listener when 1 */
    int      tls_port;                   /* TLS listener port */
    char     tls_cert_file[4096];        /* PEM certificate chain path */
    char     tls_key_file[4096];         /* PEM private key path */

    /* Phase 4: privilege drop */
    char     run_user[RUN_USER_MAX];     /* empty: keep invoking uid */
    char     run_group[RUN_USER_MAX];    /* empty: user's primary gid */

    /* Phase 5: observability and operations */
    int      observability_enabled;      /* 0/1; serve metrics/health/ready */
    int      syslog_enabled;             /* 0/1; mirror log records to syslog */
    char     metrics_path[OBS_PATH_MAX]; /* Prometheus endpoint path */
    char     health_path[OBS_PATH_MAX];  /* liveness endpoint path */
    char     readiness_path[OBS_PATH_MAX]; /* readiness endpoint path */

    /* Resolved input provenance (for the startup record); not operator-set. */
    char     config_path[4096];
    int      help_requested;
} ServerConfig;

/* Fill `cfg` with the compile-time defaults (the pre-configuration behavior). */
void config_defaults(ServerConfig *cfg);

/*
 * Apply the configuration file, environment, and command line onto `cfg` in
 * precedence order. `argc`/`argv` are the process arguments (argv[0] excluded
 * by the callee).
 *
 * Returns 0 on success, -1 on any invalid, unknown, or contradictory value
 * (after printing a message naming the offending key). When `--help` is seen,
 * prints usage, sets cfg->help_requested, and returns 0.
 */
int config_load(ServerConfig *cfg, int argc, char **argv);

/* Print the accepted key list and CLI usage to stdout. */
void config_print_usage(void);

/*
 * SIGHUP-driven reload of the safe runtime subset. `config_request_reload()`
 * sets an async-signal-safe flag from the handler; the event loop calls
 * `config_reload_if_requested()` outside signal context. The latter re-parses
 * the sources recorded by `config_set_reload_args()` (file + env + CLI) into
 * `out` and returns 1 when a configuration was loaded, 0 when none was
 * requested, and -1 when the new configuration is invalid (the previous one
 * stays in effect). Only additive, live-safe fields are applied by the caller
 * (currently the log level).
 */
void config_request_reload(void);
void config_set_reload_args(int argc, char **argv);
int  config_reload_if_requested(ServerConfig *out);

/* Human-readable name for a log level ("error"/"warn"/"info"/"debug"). */
const char *log_level_name(LogLevel level);

#endif /* CONFIG_H */
