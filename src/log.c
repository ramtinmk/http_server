#define _GNU_SOURCE   /* pipe2, F_SETPIPE_SZ */

#include "log.h"
#include "server_config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 4096   /* <= PIPE_BUF so one write stays atomic */

static int              g_log_fd = -1;
static int              g_owns_fd;
static char             g_log_path[4096];
static int              g_pipe_rd = -1;
static int              g_pipe_wr = -1;
static pthread_t        g_writer;
static int              g_writer_started;
static _Atomic int      g_stop;
static _Atomic long long g_dropped;
static LogLevel         g_max_level = LOG_LEVEL_INFO;
static int              g_access_enabled;

/* Set from a signal handler; polled by the writer thread. */
static volatile sig_atomic_t g_reopen_requested;

/* Append printf output at `off`, clamping safely; returns the new offset. */
static size_t put(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off >= cap)
        return off + 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    return off + (size_t)n;
}

static void format_timestamp(char *buf, size_t cap)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(buf, cap, "0");
        return;
    }
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);

    unsigned ms = (unsigned)(ts.tv_nsec / 1000000L);
    if (ms > 999)
        ms = 999;
    snprintf(buf, cap, "%s.%03uZ", base, ms);
}

/* Escape `in` as a JSON string body into `out`; always NUL-terminates. */
static void json_escape(const char *in, size_t inlen, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < inlen && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        char ubuf[8];
        const char *rep = NULL;
        switch (c) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
                rep = ubuf;
            }
            break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l >= cap)
                break;
            memcpy(out + o, rep, l);
            o += l;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/*
 * Hot path: write a complete line to the pipe without ever blocking. The pipe
 * is O_NONBLOCK; a full pipe is a drop, not a stall.
 */
static void log_emit(const char *line, size_t len)
{
    if (g_pipe_wr < 0 || len == 0)
        return;
    if (len > LOG_LINE_MAX)
        len = LOG_LINE_MAX;

    ssize_t n = write(g_pipe_wr, line, len);
    if (n < 0 && errno == EINTR)
        n = write(g_pipe_wr, line, len);
    if (n < 0) {
        atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
        return;
    }
    if ((size_t)n != len) {
        /* Should not happen for a line <= PIPE_BUF; count the rest as a drop
         * rather than emit a partial record. */
        atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
    }
}

void log_msg(LogLevel level, const char *fmt, ...)
{
    if (level > g_max_level || g_pipe_wr < 0)
        return;

    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    char msg[2200];
    json_escape(text, strlen(text), msg, sizeof(msg));

    char ts[40];
    format_timestamp(ts, sizeof(ts));

    char line[LOG_LINE_MAX];
    size_t off = put(line, sizeof(line), 0,
                     "{\"ts\":\"%s\",\"level\":\"%s\",\"kind\":\"server\","
                     "\"msg\":\"%s\"}\n",
                     ts, log_level_name(level), msg);
    if (off >= sizeof(line))
        off = sizeof(line) - 1;
    log_emit(line, off);
}

void log_access(const char *client, const char *method, const char *path,
                int status, size_t bytes, long latency_us)
{
    if (!g_access_enabled || g_pipe_wr < 0)
        return;

    char ts[40];
    format_timestamp(ts, sizeof(ts));

    char esc_client[128];
    char esc_method[32];
    char esc_path[1200];
    json_escape(client ? client : "-", strlen(client ? client : "-"),
                esc_client, sizeof(esc_client));
    json_escape(method ? method : "-", strlen(method ? method : "-"),
                esc_method, sizeof(esc_method));
    json_escape(path ? path : "-", strlen(path ? path : "-"),
                esc_path, sizeof(esc_path));

    char line[LOG_LINE_MAX];
    size_t off = put(line, sizeof(line), 0,
                     "{\"ts\":\"%s\",\"level\":\"info\",\"kind\":\"access\","
                     "\"client\":\"%s\",\"method\":\"%s\",\"path\":\"%s\","
                     "\"status\":%d,\"bytes\":%zu,\"latency_us\":%ld}\n",
                     ts, esc_client, esc_method, esc_path, status, bytes,
                     latency_us);
    if (off >= sizeof(line))
        off = sizeof(line) - 1;
    log_emit(line, off);
}

long long log_dropped_total(void)
{
    return atomic_load_explicit(&g_dropped, memory_order_relaxed);
}

void log_request_reopen(void)
{
    g_reopen_requested = 1;
}

/* --- Writer thread ------------------------------------------------------ */

static int open_target(void)
{
    if (g_log_path[0] == '\0') {
        g_log_fd = STDERR_FILENO;
        g_owns_fd = 0;
        return 0;
    }
    int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    g_log_fd = fd;
    g_owns_fd = 1;
    return 0;
}

static void reopen_target(void)
{
    if (g_log_path[0] == '\0')
        return;
    int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "WARNING: log reopen failed for %s: %s\n",
                g_log_path, strerror(errno));
        return;
    }
    if (g_owns_fd && g_log_fd >= 0)
        close(g_log_fd);
    g_log_fd = fd;
    g_owns_fd = 1;
}

static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return; /* best effort: dropping log output beats stalling */
        }
    }
}

static void *writer_main(void *arg)
{
    (void)arg;
    char buf[65536];

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        struct pollfd pfd = { .fd = g_pipe_rd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, LOG_POLL_INTERVAL_MS);
        if (r > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(g_pipe_rd, buf, sizeof(buf));
            if (n > 0)
                write_all(g_log_fd, buf, (size_t)n);
            continue;
        }
        if (g_reopen_requested) {
            g_reopen_requested = 0;
            reopen_target();
        }
    }

    /* Drain anything queued before exit. */
    for (;;) {
        ssize_t n = read(g_pipe_rd, buf, sizeof(buf));
        if (n <= 0)
            break;
        write_all(g_log_fd, buf, (size_t)n);
    }
    return NULL;
}

int log_init(const ServerConfig *cfg)
{
    g_max_level = cfg->log_level;
    g_access_enabled = cfg->access_log;
    snprintf(g_log_path, sizeof(g_log_path), "%s", cfg->log_file);

    if (open_target() != 0) {
        fprintf(stderr, "FATAL: cannot open log file '%s' (key 'log_file'): "
                        "%s\n", cfg->log_file, strerror(errno));
        return -1;
    }

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) {
        perror("pipe2 log");
        if (g_owns_fd && g_log_fd >= 0)
            close(g_log_fd);
        g_log_fd = -1;
        return -1;
    }
    int flags = fcntl(pfd[1], F_GETFL, 0);
    if (flags >= 0)
        fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK);
    /* The reader is nonblocking as well so the shutdown drain cannot block on
     * an empty pipe after the writer loop exits. */
    flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
#ifdef F_SETPIPE_SZ
    (void)fcntl(pfd[1], F_SETPIPE_SZ, 1 << 20);
#endif
    g_pipe_rd = pfd[0];
    g_pipe_wr = pfd[1];

    if (pthread_create(&g_writer, NULL, writer_main, NULL) != 0) {
        perror("pthread_create log writer");
        close(g_pipe_rd);
        close(g_pipe_wr);
        g_pipe_rd = g_pipe_wr = -1;
        if (g_owns_fd && g_log_fd >= 0)
            close(g_log_fd);
        g_log_fd = -1;
        return -1;
    }
    g_writer_started = 1;
    return 0;
}

void log_shutdown(void)
{
    if (!g_writer_started) {
        if (g_owns_fd && g_log_fd >= 0)
            close(g_log_fd);
        g_log_fd = -1;
        return;
    }
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    pthread_join(g_writer, NULL);
    if (g_pipe_wr >= 0)
        close(g_pipe_wr);
    if (g_pipe_rd >= 0)
        close(g_pipe_rd);
    if (g_owns_fd && g_log_fd >= 0)
        close(g_log_fd);
    g_pipe_rd = g_pipe_wr = -1;
    g_log_fd = -1;
    g_writer_started = 0;
}
