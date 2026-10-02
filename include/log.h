#ifndef LOG_H
#define LOG_H

#include "config.h"

/*
 * Leveled JSON logging with a nonblocking hot path.
 *
 * Hot-path callers (the event loops) format one JSON line and write it to an
 * O_NONBLOCK pipe. The write is atomic for lines up to PIPE_BUF and never
 * blocks: when the pipe is full the line is dropped and a counter is bumped.
 * A dedicated writer thread drains the pipe to the configured log file (or
 * stderr when none is configured), so disk I/O is isolated from the event
 * loops.
 *
 * SIGHUP semantics: call log_request_reopen() from the signal handler; the
 * writer thread reopens the file on its next poll tick.
 */

/* Open the target, create the pipe, and start the writer thread. Returns 0 on
 * success; -1 when a configured log file cannot be opened (fatal at startup). */
int log_init(const ServerConfig *cfg);

/* Flush what is queued and stop the writer thread. Safe to call once. */
void log_shutdown(void);

/* Request a reopen of the log file on the next writer tick (SIGHUP). */
void log_request_reopen(void);

/* Emit one server record at `level` (printf-style). Suppressed when `level`
 * is above the configured log level. */
void log_msg(LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Emit one access record for a completed response. No-op unless access logging
 * is enabled. `latency_us` is the response time in microseconds; `request_id`
 * is the correlation id returned in the response (may be NULL). */
void log_access(const char *client, const char *method, const char *path,
                int status, size_t bytes, long latency_us,
                const char *request_id);

/* Change the maximum emitted level at runtime (SIGHUP safe tunable). */
void log_set_level(LogLevel level);

/* Cumulative count of records dropped because the pipe was full. */
long long log_dropped_total(void);

#endif /* LOG_H */
