#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

/*
 * Lightweight, lock-free server instrumentation.
 *
 * The counters below are the server-side half of the Measurement Contract in
 * plans/scaling-plan.md. They are cheap enough to update on every request
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

/* --- Active-connection gauge -------------------------------------------- */

/* Increment the live active-connection gauge (called in the accept loop after
 * successful admission; never from worker threads). */
void metrics_active_connection_inc(void);

/* Decrement the live active-connection gauge (called from the worker thread
 * after closing the client socket). */
void metrics_active_connection_dec(void);

/* Read the current live active-connection count (used by the accept loop for
 * the admission check; the accept loop is single-threaded so no CAS needed). */
long metrics_active_connection_count(void);

/* --- Overload and limit counters ---------------------------------------- */

/* Why a new connection could not be admitted. Kept as an explicit enum so a
 * saturation run can distinguish a genuine process-wide capacity limit from a
 * per-loop table that ran out of slots. */
typedef enum {
    ADMISSION_REJECT_CAPACITY = 0, /* process-wide active-connection cap hit */
    ADMISSION_REJECT_TABLE_FULL,   /* owning loop's connection table is full */
    ADMISSION_REJECT_REASON_COUNT
} AdmissionRejectReason;

/* New connection rejected because the effective capacity was reached. */
void metrics_admission_rejected(void);

/* New connection rejected, filed under an explicit reason (also bumps the
 * aggregate admission-rejected total). */
void metrics_admission_rejected_reason(AdmissionRejectReason reason);

/* Worker acquired no buffer because the buffer pool was empty. */
void metrics_buffer_pool_exhausted(void);

/* Connection closed because no complete headers arrived within
 * HEADER_READ_TIMEOUT_SEC after accept. */
void metrics_header_timeout(void);

/* Keep-alive connection closed because the client was idle longer than
 * IDLE_TIMEOUT_SEC between requests. */
void metrics_idle_timeout(void);

/* Response write aborted because the client did not drain the socket within
 * WRITE_TIMEOUT_SEC. */
void metrics_write_timeout(void);

/* Connection closed because the unprocessed input buffer exceeded
 * MAX_INPUT_BUFFER_BYTES. */
void metrics_input_buffer_limit(void);

/* --- Phase 3 event-loop counters --------------------------------------- */

/* Incremented each time epoll_wait returns (whether events or timeout). */
void metrics_el_wakeup(void);

/* Incremented each time a readable (EPOLLIN) event is dispatched. */
void metrics_el_readable_event(void);

/* Incremented each time a writable (EPOLLOUT) event is dispatched. */
void metrics_el_writable_event(void);

/* Incremented each time recv or send returns EAGAIN/EWOULDBLOCK. */
void metrics_el_eagain(void);

/* Incremented each time a send completes a short (partial) write. */
void metrics_el_partial_write(void);

/* Incremented each time a connection is closed by the deadline scanner. */
void metrics_el_deadline_close(void);

/* Incremented each time the per-connection pipeline queue is full. */
void metrics_el_pipeline_full(void);

/* Incremented each time the output queue drains and the connection
 * transitions back to keep-alive (waiting for the next request). */
void metrics_el_output_drained(void);

/* Incremented each time a new connection is registered with epoll. */
void metrics_el_connection_opened(void);

/* Incremented each time any connection is removed from epoll and closed. */
void metrics_el_connection_closed(void);

/* Per-loop breakdown of the aggregate event-loop counters. `loop_id` is the
 * 0-based loop index; out-of-range ids are ignored. The snapshot emits the
 * first METRICS_MAX_EL_LOOPS entries as arrays. */
void metrics_el_loop_count(int count);
void metrics_el_loop_wakeup(int loop_id);
void metrics_el_loop_accepted(int loop_id);

/* --- Phase 4 saturate / backpressure counters --------------------------- */

/* Record the effective connection capacity derived at startup. Emitted in the
 * snapshot so a benchmark row can be interpreted without the server logs. */
void metrics_set_connection_capacity(long capacity);

/* Try to reserve one connection slot under a process-wide capacity. Returns
 * nonzero on success. Used by the multi-loop admission path so the global cap
 * is enforced atomically rather than per loop. */
int metrics_connection_admit(long capacity);

/* Listener backpressure: the event loop disables accept interest while the
 * connection table is full and re-enables it after a close. The first call
 * records a new disabled transition; the second records the disabled
 * duration in milliseconds. */
void metrics_listener_disabled(void);
void metrics_listener_enabled(long disabled_ms);

/* A complete 503 Service Unavailable response was returned to an overloaded
 * client instead of silently resetting the connection. */
void metrics_overload_response(void);

/* A connection was closed in a way that may reach the peer as a reset (it
 * could not be drained or answered). No silent per-request reset should be
 * counted here during normal backpressure. */
void metrics_connection_reset(void);

/* Current and maximum bytes retained by per-connection input buffers. Held by
 * the event loop; released when an idle keep-alive connection drops its
 * buffer. */
void metrics_buffer_leased(size_t bytes);
void metrics_buffer_returned(size_t bytes);

/* Backlog pressure: number of connections waiting in the kernel accept queue
 * while listener read interest is disabled. Sampled by the event loop; the
 * high-water mark shows how many clients were held by backpressure. Negative
 * samples (unsupported) are ignored. */
void metrics_backlog_depth(long depth);

/* --- Phase 5: listener drops and accept errors -------------------------- */

/* Cumulative accepted-connection counter dropped by the kernel because the
 * listener's accept queue overflowed. Sourced (Linux) from the SYN/accept
 * drop fields Netlink exposes for the listening socket and sampled by the
 * event loop; unsupported platforms simply never increment it. */
void metrics_listen_drops(void);

/* One accept()/accept4() failure filed by errno value. `errno_value` is the
 * raw errno (EMFILE, ENFILE, ECONNABORTED, ...); the snapshot emits the
 * counters that matter for diagnosing overload. EAGAIN/EINTR are normal
 * nonblocking outcomes and are not counted here. */
void metrics_accept_error(int errno_value);

#endif /* METRICS_H */
