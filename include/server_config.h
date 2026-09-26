#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

/*
 * Server configuration limits.
 *
 * All limits are documented here so a benchmark result can be reproduced
 * from the values recorded in startup output and metrics snapshots.
 *
 * Every value can be overridden at compile time:
 *   cc -DMAX_ACTIVE_CONNECTIONS=256 ...
 */

/* --- Connection admission ----------------------------------------------- */

/* Maximum simultaneously open (accepted and not yet closed) connections.
 * Must be <= (RLIMIT_NOFILE - REQUIRED_NOFILE_HEADROOM). */
#ifndef MAX_ACTIVE_CONNECTIONS
#define MAX_ACTIVE_CONNECTIONS 1024
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
 * freeing the connection slot. */
#ifndef WRITE_TIMEOUT_SEC
#define WRITE_TIMEOUT_SEC 10
#endif

/* --- File-descriptor requirements --------------------------------------- */

/* Minimum file-descriptor headroom required above MAX_ACTIVE_CONNECTIONS:
 * stdin/stdout/stderr (3), listening socket (1), metrics file (1), spare (10). */
#ifndef REQUIRED_NOFILE_HEADROOM
#define REQUIRED_NOFILE_HEADROOM 64
#endif

/* --- Phase 5: OS and deployment preflight ------------------------------- */

/*
 * Extra descriptors reserved per event-loop thread on top of
 * REQUIRED_NOFILE_HEADROOM.  Each loop owns an epoll fd, a wake eventfd, and
 * a SO_REUSEPORT listener (the first loop reuses the primary listener).  The
 * startup preflight sizes the required soft RLIMIT_NOFILE as
 *   MAX_ACTIVE_CONNECTIONS + REQUIRED_NOFILE_HEADROOM + per-loop fds,
 * so a host with many cores does not under-reserve descriptors.
 */
#ifndef REQUIRED_NOFILE_PER_LOOP
#define REQUIRED_NOFILE_PER_LOOP 3
#endif

/*
 * Paths under /proc/sys that Phase 5 reads to verify the host can honor the
 * configured backlog and TCP buffer limits.  The server only reads and
 * reports these; it never mutates host state.
 */
#define PROC_SOMAXCONN   "/proc/sys/net/core/somaxconn"
#define PROC_TCP_RMEM    "/proc/sys/net/ipv4/tcp_rmem"
#define PROC_TCP_WMEM    "/proc/sys/net/ipv4/tcp_wmem"

/*
 * Environment variable holding a CPU affinity list for the server, in the
 * same form as taskset(1) / sched_setaffinity(2) (e.g. "0-3" or "0,2,4").
 * Unset by default: enabling affinity is only justified by a measured result,
 * and the applied mask is printed at startup for the record.
 */
#define ENV_CPU_SET "HTTP_SERVER_CPU_SET"

/* --- Event loop --------------------------------------------------------- */

/* Maximum epoll events retrieved per epoll_wait call. */
#ifndef EL_MAX_EVENTS
#define EL_MAX_EVENTS 256
#endif

/* How often (ms) to scan for expired connection deadlines. */
#ifndef EL_DEADLINE_SCAN_MS
#define EL_DEADLINE_SCAN_MS 100
#endif

/*
 * Phase 4: number of event-loop threads.
 *
 * 0 (the default) auto-detects the number of online CPU cores at runtime and
 * starts one loop per core, so the server scales with the host without a
 * rebuild. A positive value is an explicit override: 1 preserves the Phase 3
 * single-loop control path exactly, and values > 1 create one SO_REUSEPORT
 * listener per loop so the kernel hashes new connections across loops; each
 * loop exclusively owns the connections it accepts (see
 * plans/scaling-plan-phase4.md section C). The override is a compile-time
 * bound; the runtime core count is the effective value when set to 0.
 */
#ifndef EL_THREAD_COUNT
#define EL_THREAD_COUNT 0
#endif

/*
 * Upper bound on the auto-detected loop count (and clamp for an explicit
 * override). Caps thread and SO_REUSEPORT listener growth on very large hosts.
 */
#ifndef EL_MAX_THREADS
#define EL_MAX_THREADS 64
#endif

/*
 * Environment variable naming the operator-configured maximum number of
 * simultaneously active connections. The effective capacity is the minimum of
 * this value and the descriptor-derived capacity printed at startup. When
 * unset it defaults to MAX_ACTIVE_CONNECTIONS.
 */
#define ENV_MAX_CONNECTIONS "HTTP_SERVER_MAX_CONNECTIONS"

/*
 * Hard upper bound on the runtime connection table regardless of the
 * descriptor limit or operator configuration. Each slot retains a fixed
 * per-connection state object, so this bounds worst-case table memory
 * independently of RLIMIT_NOFILE. Raise only with a matching memory budget.
 */
#ifndef EL_MAX_CONNECTION_TABLE
#define EL_MAX_CONNECTION_TABLE 65536
#endif

/* --- Allocator ---------------------------------------------------------- */

/*
 * glibc reserves virtual address space per malloc arena (up to ~64 MB per
 * arena on 64-bit). Left unbounded, VmSize grows with the event-loop thread
 * count even though RSS stays flat. 2 bounds the reservation while keeping one
 * secondary arena for allocator contention; 1 minimizes VmSize further at the
 * cost of serializing every allocation on the main arena. A pre-set
 * MALLOC_ARENA_MAX is respected; ENV_MALLOC_ARENA_MAX overrides this default
 * when neither is set.
 */
#ifndef MALLOC_ARENA_MAX_DEFAULT
#define MALLOC_ARENA_MAX_DEFAULT 2
#endif

#define ENV_MALLOC_ARENA_MAX "HTTP_SERVER_MALLOC_ARENA_MAX"

#endif /* SERVER_CONFIG_H */
