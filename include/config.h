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

/* Human-readable name for a log level ("error"/"warn"/"info"/"debug"). */
const char *log_level_name(LogLevel level);

#endif /* CONFIG_H */
