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

/* --- Byte ranges --------------------------------------------------------- */

/* Maximum number of satisfiable ranges assembled into one
 * `multipart/byteranges` response. A range request with more satisfiable ranges
 * is answered with the full 200 body instead (RFC 9110 permits ignoring a
 * Range header), bounding per-request work and response size. */
#ifndef MAX_MULTIPART_RANGES
#define MAX_MULTIPART_RANGES 8
#endif

/* Maximum size (bytes) of an assembled `multipart/byteranges` body, including
 * boundaries and part headers. A request whose body would exceed this is
 * answered with the full 200 body instead. Bounds the heap a single range
 * response can request. */
#ifndef MAX_MULTIPART_BYTES
#define MAX_MULTIPART_BYTES 8192
#endif

/* --- Phase 2: static file serving --------------------------------------- */

/*
 * Directory served for request paths that are not one of the legacy fixed-path
 * aliases. Every resolved path must stay beneath this directory. Relative paths
 * are resolved against the process working directory, so the server must be
 * launched from the repository root (or given an absolute root).
 */
#ifndef DOCUMENT_ROOT
#define DOCUMENT_ROOT "."
#endif

/*
 * Comma-separated index file names tried, in order, when a request resolves to
 * a directory. No directory listing is ever produced; when none of the index
 * files exists the directory is a 404.
 */
#ifndef INDEX_FILES
#define INDEX_FILES "index.html,index.htm"
#endif

/*
 * Maximum size of a file that is read into the bounded representation cache.
 * Larger files are streamed straight from the file descriptor (identity
 * encoding only), so a cache entry can never exceed this many bytes. Bounds
 * both per-entry memory and the time a single cache fill can block.
 */
#ifndef CACHE_MAX_FILE_BYTES
#define CACHE_MAX_FILE_BYTES (1024u * 1024u)
#endif

/*
 * Hard upper bound on the number of entries in the representation cache,
 * independent of the operator byte budget. Bounds the eviction scan and the
 * per-entry metadata when many tiny files are requested.
 */
#ifndef CACHE_MAX_ENTRIES
#define CACHE_MAX_ENTRIES 256
#endif

/* Default total byte budget for the representation cache (16 MiB). A runtime
 * budget of 0 disables caching and streams every file from its descriptor. */
#ifndef CACHE_BUDGET_BYTES_DEFAULT
#define CACHE_BUDGET_BYTES_DEFAULT (16u * 1024u * 1024u)
#endif

/* --- Phase 3: TLS termination ------------------------------------------- */

/* Default port for the optional TLS listener. Plaintext stays on PORT; the two
 * listeners are independent and must not share a port. */
#ifndef TLS_PORT_DEFAULT
#define TLS_PORT_DEFAULT 8443
#endif

/*
 * Per-connection buffer used to stream a file-backed response body over TLS.
 * TLS cannot use sendfile(2), so the event loop reads a chunk from the file
 * descriptor and writes it through the TLS record layer. A fixed-size buffer
 * bounds the extra memory each in-flight streaming TLS response can hold.
 */
#ifndef TLS_FILE_BUF_SIZE
#define TLS_FILE_BUF_SIZE 16384
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

/* --- Graceful shutdown --------------------------------------------------- */

/* On SIGTERM/SIGINT the server stops accepting and finishes in-flight
 * responses for at most this many seconds before force-closing what remains.
 * Bounds shutdown time so a stuck peer cannot delay process exit. */
#ifndef SHUTDOWN_DRAIN_TIMEOUT_SEC
#define SHUTDOWN_DRAIN_TIMEOUT_SEC 10
#endif

/* Timeout (ms) for a log-writer poll before it re-checks the SIGHUP reopen
 * flag. Keeps reopen latency bounded without busy-waiting. */
#ifndef LOG_POLL_INTERVAL_MS
#define LOG_POLL_INTERVAL_MS 200
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

/* --- Runtime configuration surface -------------------------------------- */

/*
 * Environment variable names and a key=value file plus CLI flags all feed one
 * validated configuration (see include/config.h). Precedence is
 * defaults < config file < environment < command line; an invalid value names
 * the offending key and is fatal.
 */
#define ENV_CONFIG                  "HTTP_SERVER_CONFIG"

/*
 * Config file loaded when neither --config PATH nor HTTP_SERVER_CONFIG is set.
 * CMakeLists.txt points this at the checked-in http_server.conf via
 * -DDEFAULT_CONFIG_FILE=...; an empty string disables the fallback so the
 * server runs on pure compiled defaults.
 */
#ifndef DEFAULT_CONFIG_FILE
#define DEFAULT_CONFIG_FILE "http_server.conf"
#endif
#define ENV_PORT                    "HTTP_SERVER_PORT"
#define ENV_BACKLOG                 "HTTP_SERVER_BACKLOG"
#define ENV_MAX_KEEPALIVE_REQUESTS  "HTTP_SERVER_MAX_KEEPALIVE_REQUESTS"
#define ENV_MAX_INPUT_BUFFER_BYTES  "HTTP_SERVER_MAX_INPUT_BUFFER_BYTES"
#define ENV_HEADER_READ_TIMEOUT     "HTTP_SERVER_HEADER_READ_TIMEOUT"
#define ENV_IDLE_TIMEOUT            "HTTP_SERVER_IDLE_TIMEOUT"
#define ENV_WRITE_TIMEOUT           "HTTP_SERVER_WRITE_TIMEOUT"
#define ENV_SHUTDOWN_DRAIN_TIMEOUT  "HTTP_SERVER_SHUTDOWN_DRAIN_TIMEOUT"
#define ENV_LOG_LEVEL               "HTTP_SERVER_LOG_LEVEL"
#define ENV_LOG_FILE                "HTTP_SERVER_LOG_FILE"
#define ENV_ACCESS_LOG              "HTTP_SERVER_ACCESS_LOG"

/* Phase 2: secure static file serving. */
#define ENV_DOCUMENT_ROOT           "HTTP_SERVER_DOCUMENT_ROOT"
#define ENV_INDEX_FILES             "HTTP_SERVER_INDEX_FILES"
#define ENV_MIME_TYPES              "HTTP_SERVER_MIME_TYPES"
#define ENV_HIDDEN_FILES            "HTTP_SERVER_HIDDEN_FILES"
#define ENV_SYMLINKS                "HTTP_SERVER_SYMLINKS"
#define ENV_CACHE_BUDGET_BYTES      "HTTP_SERVER_CACHE_BUDGET_BYTES"

/* Phase 3: TLS termination. */
#define ENV_TLS                     "HTTP_SERVER_TLS"
#define ENV_TLS_PORT                "HTTP_SERVER_TLS_PORT"
#define ENV_TLS_CERT_FILE           "HTTP_SERVER_TLS_CERT"
#define ENV_TLS_KEY_FILE            "HTTP_SERVER_TLS_KEY"

/* --- Event loop --------------------------------------------------------- */

/* Maximum accepted sockets processed by one listener event dispatch. Under
 * sustained overload, a finite batch keeps rejection work from monopolizing
 * the owning event loop; level-triggered epoll schedules the listener again. */
#ifndef EL_ACCEPT_BATCH_SIZE
#define EL_ACCEPT_BATCH_SIZE 64
#endif

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
