#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "http_server.h"  /* PendingResponse is defined there */
#include "ring_buffer.h"
#include <time.h>
#include <signal.h>

/* Maximum epoll events retrieved per epoll_wait call. */
#ifndef EL_MAX_EVENTS
#define EL_MAX_EVENTS 256
#endif

/* How often (ms) to scan for expired connection deadlines. */
#ifndef EL_DEADLINE_SCAN_MS
#define EL_DEADLINE_SCAN_MS 100
#endif

/* ------------------------------------------------------------------ */
/* Connection lifecycle states                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    CONN_READING_HEADERS,   /* Waiting for first complete request headers. */
    CONN_KEEP_ALIVE,        /* Between requests; waiting for next request.  */
    CONN_WRITING,           /* Sending response data.                       */
    CONN_CLOSING            /* Pending close; do not register new events.   */
} ELConnState;

/* ------------------------------------------------------------------ */
/* Close reason codes                                                   */
/* ------------------------------------------------------------------ */
typedef enum {
    CLOSE_CLIENT_EOF = 0,
    CLOSE_DEADLINE,
    CLOSE_PROTOCOL_ERROR,
    CLOSE_WRITE_ERROR,
    CLOSE_PIPELINE_LIMIT,
    CLOSE_KEEPALIVE_LIMIT,
    CLOSE_BUFFER_FULL,
    CLOSE_SHUTDOWN
} ELCloseReason;

/* ------------------------------------------------------------------ */
/* Per-connection state owned exclusively by the event loop.            */
/* PendingResponse is defined in http_server.h.                         */
/* ------------------------------------------------------------------ */
typedef struct ELConnection {
    int            fd;               /* Client fd, or -1 if free.           */
    ELConnState    state;

    /* Input */
    RingBuffer    *in_buf;           /* Owned; freed on close.              */

    /* Pipeline queue (ring buffer of pending responses) */
    PendingResponse pq[MAX_PIPELINE_DEPTH + 1];
    int            pq_head;          /* Next response to send.              */
    int            pq_tail;          /* Next free write slot.               */
    int            pq_count;         /* Responses currently queued.         */

    /* Output cursor for the current (head) response */
    size_t         out_header_sent;
    size_t         out_body_sent;

    /* HTTP session */
    int            request_count;    /* Total requests served.              */
    int            keep_alive;       /* Last request indicated keep-alive.  */

    /* Monotonic deadline (CLOCK_MONOTONIC) */
    struct timespec deadline;

    /* Diagnostics */
    ELCloseReason  close_reason;

    /* Intrusive free list linkage */
    struct ELConnection *next;
} ELConnection;

/*
 * Opaque per-loop state. One instance exists per event-loop thread; a
 * connection accepted by a loop is owned exclusively by that loop (its epoll
 * fd, connection table, free list, deadline scan, and wake eventfd).
 */
typedef struct EventLoop EventLoop;

/*
 * Run one or more event loops until *running becomes 0.
 *
 * server_fd must already be bound and listening; when EL_THREAD_COUNT > 1 the
 * additional loops create their own SO_REUSEPORT listeners on the same port
 * (server_fd must therefore also have been bound with SO_REUSEPORT).
 *
 * `capacity` is the process-wide maximum number of simultaneously active
 * connections derived at startup. Admission is enforced atomically across all
 * loops, and each loop disables its listener interest while the global table
 * is full (listener backpressure).
 *
 * Returns 0 on clean shutdown, -1 on fatal error.
 */
int event_loop_run(int server_fd, volatile sig_atomic_t *running, long capacity);

#endif /* EVENT_LOOP_H */
