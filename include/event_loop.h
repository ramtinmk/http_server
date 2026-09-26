#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "http_server.h"  /* PendingResponse is defined there */
#include "ring_buffer.h"
#include <time.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* Connection lifecycle states                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    CONN_READING_HEADERS,   /* Waiting for first complete request headers. */
    CONN_KEEP_ALIVE,        /* Between requests; waiting for next request.  */
    CONN_WRITING,           /* Sending response data.                       */
} ELConnState;

/* ------------------------------------------------------------------ */
/* Close reason codes                                                   */
/* ------------------------------------------------------------------ */
typedef enum {
    CLOSE_CLIENT_EOF = 0,
    CLOSE_DEADLINE,
    CLOSE_PROTOCOL_ERROR,
    CLOSE_WRITE_ERROR,
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
 * Resolve the number of event-loop threads to start.
 *
 * Returns EL_THREAD_COUNT when it was set to a positive value (explicit
 * compile-time override); otherwise queries the number of online CPU cores
 * (sysconf(_SC_NPROCESSORS_ONLN)) and clamps it to [1, EL_MAX_THREADS].
 * Never returns less than 1.
 */
int event_loop_thread_count(void);

/*
 * Run `nloops` event loops until *running becomes 0.
 *
 * server_fd must already be bound and listening; when nloops > 1 the
 * additional loops create their own SO_REUSEPORT listeners on the same port
 * (server_fd must therefore also have been bound with SO_REUSEPORT). Passing
 * nloops < 1 is treated as 1.
 *
 * `capacity` is the process-wide maximum number of simultaneously active
 * connections derived at startup. Admission is enforced atomically across all
 * loops, and each loop disables its listener interest while the global table
 * is full (listener backpressure).
 *
 * Returns 0 on clean shutdown, -1 on fatal error.
 */
int event_loop_run(int server_fd, volatile sig_atomic_t *running, long capacity,
                   int nloops);

#endif /* EVENT_LOOP_H */
