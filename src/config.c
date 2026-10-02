#include "config.h"
#include "server_config.h"
#include "http_server.h"   /* PORT, BACKLOG */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CFG_OFF(field)  offsetof(ServerConfig, field)
#define CFG_SIZE(field) sizeof(((ServerConfig *)0)->field)

typedef enum {
    CFG_INT,
    CFG_LONG,
    CFG_BOOL,
    CFG_LOGLEVEL,
    CFG_STRING
} CfgType;

typedef struct {
    const char *key;
    CfgType     type;
    size_t      offset;
    size_t      strcap;   /* CFG_STRING only */
    long        min;
    long        max;
    const char *env;
} CfgKey;

/*
 * The whole runtime surface in one table. Adding a limit means adding one row
 * here and a default in config_defaults(); everything else (file, env, CLI,
 * validation, usage) follows.
 */
static const CfgKey CFG_KEYS[] = {
    { "port",                    CFG_INT,      CFG_OFF(port), 0, 1, 65535,
      ENV_PORT },
    { "backlog",                 CFG_INT,      CFG_OFF(backlog), 0, 1, INT_MAX,
      ENV_BACKLOG },
    { "max_connections",         CFG_LONG,     CFG_OFF(max_connections), 0, 1,
      LONG_MAX, ENV_MAX_CONNECTIONS },
    { "max_keepalive_requests",  CFG_INT,      CFG_OFF(max_keepalive_requests),
      0, 1, INT_MAX, ENV_MAX_KEEPALIVE_REQUESTS },
    { "max_input_buffer_bytes",  CFG_INT,      CFG_OFF(max_input_buffer_bytes),
      0, 1024, INT_MAX, ENV_MAX_INPUT_BUFFER_BYTES },
    { "header_read_timeout",     CFG_INT,      CFG_OFF(header_read_timeout_sec),
      0, 1, 86400, ENV_HEADER_READ_TIMEOUT },
    { "idle_timeout",            CFG_INT,      CFG_OFF(idle_timeout_sec),
      0, 1, 86400, ENV_IDLE_TIMEOUT },
    { "write_timeout",           CFG_INT,      CFG_OFF(write_timeout_sec),
      0, 1, 86400, ENV_WRITE_TIMEOUT },
    { "shutdown_drain_timeout",  CFG_INT,
      CFG_OFF(shutdown_drain_timeout_sec), 0, 1, 86400,
      ENV_SHUTDOWN_DRAIN_TIMEOUT },
    { "log_level",               CFG_LOGLEVEL, CFG_OFF(log_level), 0,
      LOG_LEVEL_ERROR, LOG_LEVEL_DEBUG, ENV_LOG_LEVEL },
    { "access_log",              CFG_BOOL,     CFG_OFF(access_log), 0, 0, 1,
      ENV_ACCESS_LOG },
    { "log_file",                CFG_STRING,   CFG_OFF(log_file),
      CFG_SIZE(log_file), 0, 0, ENV_LOG_FILE },
    { "document_root",           CFG_STRING,   CFG_OFF(document_root),
      CFG_SIZE(document_root), 0, 0, ENV_DOCUMENT_ROOT },
    { "index_files",             CFG_STRING,   CFG_OFF(index_files),
      CFG_SIZE(index_files), 0, 0, ENV_INDEX_FILES },
    { "mime_types",              CFG_STRING,   CFG_OFF(mime_types_file),
      CFG_SIZE(mime_types_file), 0, 0, ENV_MIME_TYPES },
    { "hidden_files",            CFG_BOOL,     CFG_OFF(hidden_files_allowed),
      0, 0, 1, ENV_HIDDEN_FILES },
    { "symlinks",                CFG_BOOL,     CFG_OFF(symlinks_allowed),
      0, 0, 1, ENV_SYMLINKS },
    { "cache_budget_bytes",      CFG_LONG,     CFG_OFF(cache_budget_bytes),
      0, 0, LONG_MAX, ENV_CACHE_BUDGET_BYTES },
    { "tls",                     CFG_BOOL,     CFG_OFF(tls_enabled), 0, 0, 1,
      ENV_TLS },
    { "tls_port",                CFG_INT,      CFG_OFF(tls_port), 0, 1, 65535,
      ENV_TLS_PORT },
    { "tls_cert_file",           CFG_STRING,   CFG_OFF(tls_cert_file),
      CFG_SIZE(tls_cert_file), 0, 0, ENV_TLS_CERT_FILE },
    { "tls_key_file",            CFG_STRING,   CFG_OFF(tls_key_file),
      CFG_SIZE(tls_key_file), 0, 0, ENV_TLS_KEY_FILE },
    { "run_user",                CFG_STRING,   CFG_OFF(run_user),
      CFG_SIZE(run_user), 0, 0, ENV_RUN_USER },
    { "run_group",               CFG_STRING,   CFG_OFF(run_group),
      CFG_SIZE(run_group), 0, 0, ENV_RUN_GROUP },
    { "observability",           CFG_BOOL,     CFG_OFF(observability_enabled),
      0, 0, 1, ENV_OBSERVABILITY },
    { "syslog",                  CFG_BOOL,     CFG_OFF(syslog_enabled), 0, 0, 1,
      ENV_SYSLOG },
    { "metrics_path",            CFG_STRING,   CFG_OFF(metrics_path),
      CFG_SIZE(metrics_path), 0, 0, ENV_METRICS_PATH },
    { "health_path",             CFG_STRING,   CFG_OFF(health_path),
      CFG_SIZE(health_path), 0, 0, ENV_HEALTH_PATH },
    { "readiness_path",          CFG_STRING,   CFG_OFF(readiness_path),
      CFG_SIZE(readiness_path), 0, 0, ENV_READINESS_PATH },
};

#define CFG_KEY_COUNT (sizeof(CFG_KEYS) / sizeof(CFG_KEYS[0]))

void config_defaults(ServerConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port                     = PORT;
    cfg->backlog                  = BACKLOG;
    cfg->max_connections          = MAX_ACTIVE_CONNECTIONS;
    cfg->max_keepalive_requests   = MAX_KEEPALIVE_REQUESTS;
    cfg->max_input_buffer_bytes   = MAX_INPUT_BUFFER_BYTES;
    cfg->header_read_timeout_sec  = HEADER_READ_TIMEOUT_SEC;
    cfg->idle_timeout_sec         = IDLE_TIMEOUT_SEC;
    cfg->write_timeout_sec        = WRITE_TIMEOUT_SEC;
    cfg->shutdown_drain_timeout_sec = SHUTDOWN_DRAIN_TIMEOUT_SEC;
    cfg->log_level                = LOG_LEVEL_INFO;
    cfg->access_log               = 0;
    cfg->log_file[0]              = '\0';
    snprintf(cfg->document_root, sizeof(cfg->document_root), "%s", DOCUMENT_ROOT);
    snprintf(cfg->index_files, sizeof(cfg->index_files), "%s", INDEX_FILES);
    cfg->mime_types_file[0]       = '\0';
    cfg->hidden_files_allowed     = 0;
    cfg->symlinks_allowed         = 0;
    cfg->cache_budget_bytes       = (long)CACHE_BUDGET_BYTES_DEFAULT;
    cfg->tls_enabled              = 0;
    cfg->tls_port                 = TLS_PORT_DEFAULT;
    cfg->tls_cert_file[0]         = '\0';
    cfg->tls_key_file[0]          = '\0';
    cfg->run_user[0]              = '\0';
    cfg->run_group[0]             = '\0';
    cfg->observability_enabled    = 0;
    cfg->syslog_enabled           = 0;
    snprintf(cfg->metrics_path, sizeof(cfg->metrics_path), "%s",
             METRICS_PATH_DEFAULT);
    snprintf(cfg->health_path, sizeof(cfg->health_path), "%s",
             HEALTH_PATH_DEFAULT);
    snprintf(cfg->readiness_path, sizeof(cfg->readiness_path), "%s",
             READINESS_PATH_DEFAULT);
    cfg->config_path[0]           = '\0';
    cfg->help_requested           = 0;
}

const char *log_level_name(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "error";
    case LOG_LEVEL_WARN:  return "warn";
    case LOG_LEVEL_INFO:  return "info";
    case LOG_LEVEL_DEBUG: return "debug";
    }
    return "unknown";
}

/* --- Small parsing helpers ---------------------------------------------- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_bool(const char *value, long *out)
{
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_loglevel(const char *value, long *out)
{
    if (strcasecmp(value, "error") == 0) { *out = LOG_LEVEL_ERROR; return 0; }
    if (strcasecmp(value, "warn") == 0)  { *out = LOG_LEVEL_WARN;  return 0; }
    if (strcasecmp(value, "info") == 0)  { *out = LOG_LEVEL_INFO;  return 0; }
    if (strcasecmp(value, "debug") == 0) { *out = LOG_LEVEL_DEBUG; return 0; }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno == 0 && end != value && *end == '\0' &&
        parsed >= LOG_LEVEL_ERROR && parsed <= LOG_LEVEL_DEBUG) {
        *out = parsed;
        return 0;
    }
    return -1;
}

/*
 * Store one value into cfg. `source` names where the value came from (file and
 * line, environment variable, or command line) so errors point at the origin.
 * Always names the offending key.
 */
static int config_set(ServerConfig *cfg, const CfgKey *key, const char *value,
                      const char *source)
{
    char *base = (char *)cfg;

    switch (key->type) {
    case CFG_STRING: {
        size_t len = strlen(value);
        if (len >= key->strcap) {
            fprintf(stderr, "FATAL: %s: value for key '%s' is too long "
                            "(max %zu bytes)\n", source, key->key,
                    key->strcap - 1);
            return -1;
        }
        memcpy(base + key->offset, value, len + 1);
        return 0;
    }
    case CFG_BOOL: {
        long parsed = 0;
        if (parse_bool(value, &parsed) != 0) {
            fprintf(stderr, "FATAL: %s: invalid boolean '%s' for key '%s' "
                            "(expected 0/1)\n", source, value, key->key);
            return -1;
        }
        *(int *)(base + key->offset) = (int)parsed;
        return 0;
    }
    case CFG_LOGLEVEL: {
        long parsed = 0;
        if (parse_loglevel(value, &parsed) != 0) {
            fprintf(stderr, "FATAL: %s: invalid level '%s' for key '%s' "
                            "(expected error|warn|info|debug)\n",
                    source, value, key->key);
            return -1;
        }
        *(LogLevel *)(base + key->offset) = (LogLevel)parsed;
        return 0;
    }
    case CFG_INT:
    case CFG_LONG: {
        errno = 0;
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0') {
            fprintf(stderr, "FATAL: %s: invalid integer '%s' for key '%s'\n",
                    source, value, key->key);
            return -1;
        }
        if (parsed < key->min || parsed > key->max) {
            fprintf(stderr, "FATAL: %s: value %ld for key '%s' is out of range "
                            "%ld..%ld\n", source, parsed, key->key,
                    key->min, key->max);
            return -1;
        }
        if (key->type == CFG_INT)
            *(int *)(base + key->offset) = (int)parsed;
        else
            *(long *)(base + key->offset) = parsed;
        return 0;
    }
    }
    return -1;
}

static const CfgKey *config_lookup(const char *name)
{
    char normalized[128];
    size_t n = strlen(name);
    if (n >= sizeof(normalized))
        return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '-')
            c = '_';
        normalized[i] = (char)tolower((unsigned char)c);
    }
    normalized[n] = '\0';

    for (size_t i = 0; i < CFG_KEY_COUNT; i++) {
        if (strcmp(normalized, CFG_KEYS[i].key) == 0)
            return &CFG_KEYS[i];
    }
    return NULL;
}

/* --- Sources ------------------------------------------------------------ */

static int config_load_file(ServerConfig *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "FATAL: cannot open config file '%s' (key 'config'): "
                        "%s\n", path, strerror(errno));
        return -1;
    }

    char line[4096];
    int lineno = 0;
    int status = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr,
                    "FATAL: %s:%d: expected 'key = value', got '%s'\n",
                    path, lineno, s);
            status = -1;
            break;
        }
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);

        /* Allow optional surrounding single or double quotes on the value. */
        size_t vlen = strlen(value);
        if (vlen >= 2 && (value[0] == '"' || value[0] == '\'') &&
            value[vlen - 1] == value[0]) {
            value[vlen - 1] = '\0';
            value++;
        }

        const CfgKey *entry = config_lookup(key);
        if (!entry) {
            fprintf(stderr, "FATAL: %s:%d: unknown configuration key '%s'\n",
                    path, lineno, key);
            status = -1;
            break;
        }

        char source[4200];
        snprintf(source, sizeof(source), "%s:%d", path, lineno);
        if (config_set(cfg, entry, value, source) != 0) {
            status = -1;
            break;
        }
    }

    fclose(fp);
    if (status == 0)
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", path);
    return status;
}

static int config_load_env(ServerConfig *cfg)
{
    for (size_t i = 0; i < CFG_KEY_COUNT; i++) {
        const char *value = getenv(CFG_KEYS[i].env);
        if (value && *value) {
            if (config_set(cfg, &CFG_KEYS[i], value, CFG_KEYS[i].env) != 0)
                return -1;
        }
    }
    return 0;
}

static int config_load_cli(ServerConfig *cfg, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            cfg->help_requested = 1;
            return 0;
        }
        /* --config was consumed while resolving the file path. */
        if (strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "FATAL: command line: option '--config' "
                                "requires a path\n");
                return -1;
            }
            i++;
            continue;
        }
        if (strncmp(arg, "--config=", 9) == 0)
            continue;

        if (strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "FATAL: command line: unexpected argument '%s'\n",
                    arg);
            return -1;
        }

        const char *name = arg + 2;
        const char *equals = strchr(name, '=');
        char name_buf[128];
        const char *value;

        if (equals) {
            size_t len = (size_t)(equals - name);
            if (len >= sizeof(name_buf)) {
                fprintf(stderr, "FATAL: command line: option name too long\n");
                return -1;
            }
            memcpy(name_buf, name, len);
            name_buf[len] = '\0';
            value = equals + 1;
        } else {
            if (strlen(name) >= sizeof(name_buf)) {
                fprintf(stderr, "FATAL: command line: option name too long\n");
                return -1;
            }
            snprintf(name_buf, sizeof(name_buf), "%s", name);
            if (i + 1 >= argc) {
                fprintf(stderr, "FATAL: command line: option '--%s' requires "
                                "a value\n", name_buf);
                return -1;
            }
            value = argv[++i];
        }

        const CfgKey *entry = config_lookup(name_buf);
        if (!entry) {
            fprintf(stderr,
                    "FATAL: command line: unknown configuration key '%s'\n",
                    name_buf);
            return -1;
        }
        if (config_set(cfg, entry, value, "command line") != 0)
            return -1;
    }
    return 0;
}

/* --- Public entry points ------------------------------------------------ */

void config_print_usage(void)
{
    printf("Usage: http_server [--config PATH] [--key value | --key=value ...]\n\n");
    printf("Precedence: defaults < config file < environment < command line.\n");
    printf("Config file and environment accept the same keys; the environment\n");
    printf("variables are listed below. Keys may be written with '-' or '_'.\n\n");
    for (size_t i = 0; i < CFG_KEY_COUNT; i++) {
        printf("  %-24s  env: %s\n", CFG_KEYS[i].key, CFG_KEYS[i].env);
    }
    printf("\n  --config PATH             config file (default: %s)\n",
           DEFAULT_CONFIG_FILE[0] ? DEFAULT_CONFIG_FILE : "(none)");
    printf("                            env: %s\n", ENV_CONFIG);
    printf("  --help                    this message\n");
}

/* --- SIGHUP reload ------------------------------------------------------- */

static volatile sig_atomic_t g_reload_requested;
static int   g_reload_argc;
static char **g_reload_argv;

void config_request_reload(void)
{
    g_reload_requested = 1;
}

void config_set_reload_args(int argc, char **argv)
{
    g_reload_argc = argc;
    g_reload_argv = argv;
}

int config_reload_if_requested(ServerConfig *out)
{
    if (!g_reload_requested)
        return 0;
    g_reload_requested = 0;

    ServerConfig fresh;
    config_defaults(&fresh);
    if (config_load(&fresh, g_reload_argc, g_reload_argv) != 0)
        return -1; /* config_load names the offending key */
    *out = fresh;
    return 1;
}

int config_load(ServerConfig *cfg, int argc, char **argv)
{
    /* Resolve the config file path from the command line, then the
     * environment, before applying any other source so the file is the base
     * layer the later sources override. */
    char path[4096];
    path[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(path, sizeof(path), "%s", argv[i + 1]);
            break;
        }
        if (strncmp(argv[i], "--config=", 9) == 0) {
            snprintf(path, sizeof(path), "%s", argv[i] + 9);
            break;
        }
    }
    if (path[0] == '\0') {
        const char *env = getenv(ENV_CONFIG);
        if (env && *env)
            snprintf(path, sizeof(path), "%s", env);
    }
    if (path[0] == '\0' && DEFAULT_CONFIG_FILE[0] != '\0')
        snprintf(path, sizeof(path), "%s", DEFAULT_CONFIG_FILE);

    if (path[0] != '\0' && config_load_file(cfg, path) != 0)
        return -1;
    if (config_load_env(cfg) != 0)
        return -1;
    if (config_load_cli(cfg, argc, argv) != 0)
        return -1;
    return 0;
}
