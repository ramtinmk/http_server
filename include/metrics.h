#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

/*
 * Lightweight, lock-free server instrumentation.
 *
 * The counters below are the server-side half of the Measurement Contract in
 * docs/scaling-plan.md. They are cheap enough to update on every request
 * (relaxed atomics, no locks) so they do not perturb the hot path.
 *
 * When the environment variable HTTP_SERVER_METRICS_FILE is set, a background
 * reporter thread periodically writes a JSON snapshot of every counter to that
 * path. The dependency-free benchmark harness reads that file to learn the
 * server-side view (queue depth, rejected tasks, response classes, active
 * workers) without adding any HTTP endpoint or otherwise changing the public
 * surface of the server.
 *
 * Counter semantics:
 *   - accepted_connections : TCP connections returned by accept()
 *   - completed_requests   : responses actually handed to the socket layer
 *   - request_failures     : transport/protocol failures with no valid response
 *   - responses by status  : distribution across the status codes we emit
 *   - gauge counters       : instantaneous values plus a high-water mark
 */

/* --- Cumulative counters (monotonic, never reset) ----------------------- */

/* Called by the accept loop once per successfully accepted connection. */
void metrics_connection_accepted(void);

/* Called once per response that was fully framed and sent. */
void metrics_request_completed(void);

/* Called when a request dies before a valid response could be sent
 * (send/receive error, compression failure, disk read failure, ...). */
void metrics_request_failed(void);

/* Convenience helper that records one completed response and files it under
 * the status-code distribution. Prefer this over metrics_request_completed()
 * at the point where a response is sent. */
void metrics_response(int status);

/* Called when the task queue is full and a connection has to be dropped. */
void metrics_task_rejected(void);

/* --- Gauges (instantaneous values, plus tracked maximum) ---------------- */

/* Task queue depth: enqueue when a task is added, dequeue when a worker
 * takes one. The maximum observed depth is tracked for snapshot output. */
void metrics_queue_enqueued(void);
void metrics_queue_dequeued(void);

/* Worker activity: busy when a worker starts handling a connection, idle when
 * it finishes. The maximum concurrent worker count is tracked. */
void metrics_worker_busy(void);
void metrics_worker_idle(void);

/* --- Snapshot reporter -------------------------------------------------- */

/* Start a detached thread that writes a JSON snapshot to `path` every
 * `interval_ms` milliseconds (values <= 0 fall back to 1000 ms). Safe to call
 * more than once: only the first call starts a reporter. */
void metrics_reporter_start(const char *path, int interval_ms);

/* Ask the reporter thread to write one final snapshot and exit. */
void metrics_reporter_stop(void);

/* Nonzero while the reporter thread is running. */
int metrics_reporter_active(void);

/* Render the current counters as a single-line JSON object into `buf`.
 * Returns the number of bytes that would have been written (snprintf
 * semantics); the output is truncated if it does not fit. */
size_t metrics_snapshot(char *buf, size_t cap);

#endif /* METRICS_H */
