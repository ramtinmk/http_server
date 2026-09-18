#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

/*
 * Phase 2 resource-protection configuration.
 *
 * All limits are documented here so a benchmark result can be reproduced
 * from the values recorded in startup output and metrics snapshots.
 *
 * Every value can be overridden at compile time:
 *   cc -DMAX_ACTIVE_CONNECTIONS=256 ...
 */

/* --- Thread pool -------------------------------------------------------- */

/* Number of worker threads. Test values around available CPU count. */
#ifndef THREAD_POOL_SIZE
#define THREAD_POOL_SIZE 16
#endif

/* Maximum tasks waiting in the queue (beyond the workers actively running).
 * When this is full, new connections are rejected without queuing. */
#ifndef MAX_QUEUED_TASKS
#define MAX_QUEUED_TASKS 256
#endif

/* --- Connection admission ----------------------------------------------- */

/* Maximum simultaneously open (accepted and not yet closed) connections.
 * Must be <= (RLIMIT_NOFILE - REQUIRED_NOFILE_HEADROOM).
 * Setting this higher than THREAD_POOL_SIZE increases queue wait time
 * for connections beyond the worker count; it does not increase parallelism. */
#ifndef MAX_ACTIVE_CONNECTIONS
#define MAX_ACTIVE_CONNECTIONS 512
#endif

/* --- Per-connection limits ---------------------------------------------- */

/* Maximum HTTP requests served on one keep-alive connection before the server
 * forces Connection: close and closes the socket. */
#ifndef MAX_KEEPALIVE_REQUESTS
#define MAX_KEEPALIVE_REQUESTS 100
#endif

/* Maximum request+header bytes buffered per connection. Well-formed HTTP
 * requests are a few KB; this cap is far below the ring buffer's internal
 * 16 MB safety ceiling. Exceeded → 413. */
#ifndef MAX_INPUT_BUFFER_BYTES
#define MAX_INPUT_BUFFER_BYTES 65536  /* 64 KB */
#endif

/* Maximum pipelined requests processed from one recv() pass before requiring
 * a new read. Prevents one heavy pipelining client from starving others. */
#ifndef MAX_PIPELINE_DEPTH
#define MAX_PIPELINE_DEPTH 16
#endif

/* --- Timeouts (seconds) ------------------------------------------------- */

/* Time allowed for a new connection to deliver complete request headers.
 * Clients that send nothing within this window are closed silently. */
#ifndef HEADER_READ_TIMEOUT_SEC
#define HEADER_READ_TIMEOUT_SEC 5
#endif

/* Time a keep-alive connection may idle between requests before the server
 * closes it. Applied after each successfully served request. */
#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC 30
#endif

/* Time allowed for a full response to be written to the socket. A slow
 * receiver that cannot drain the TCP buffer within this window is closed,
 * freeing the worker. */
#ifndef WRITE_TIMEOUT_SEC
#define WRITE_TIMEOUT_SEC 10
#endif

/* --- File-descriptor requirements --------------------------------------- */

/* Minimum file-descriptor headroom required above MAX_ACTIVE_CONNECTIONS:
 * stdin/stdout/stderr (3), listening socket (1), metrics file (1), spare (10). */
#ifndef REQUIRED_NOFILE_HEADROOM
#define REQUIRED_NOFILE_HEADROOM 64
#endif

#endif /* SERVER_CONFIG_H */
