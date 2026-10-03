#include "metrics.h"
#include "memory_profiler.h"

#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/*
 * All counters use C11 relaxed atomics. The values are independent statistics
 * where a slightly stale read is acceptable, so we only need atomicity (to
 * avoid torn updates across the event-loop threads) and not ordering relative
 * to other memory. This keeps the instrumentation invisible in profiles.
 */

/* Cumulative totals (monotonic; only ever incremented). */
static _Atomic long long g_accepted_connections; /* connections accepted   */
static _Atomic long long g_completed_requests;   /* responses sent         */

/* Active-connection gauge (live connections, not cumulative). */
static _Atomic long g_active_connections;
static _Atomic long g_active_connections_max;

/* Overload and limit counters (cumulative). */
static _Atomic long long g_admission_rejected;
static _Atomic long long g_admission_rejected_by_reason[ADMISSION_REJECT_REASON_COUNT];
static _Atomic long long g_header_timeout;
static _Atomic long long g_idle_timeout;
static _Atomic long long g_write_timeout;
static _Atomic long long g_input_buffer_limit;

/* Phase 4 saturation / backpressure state. */
static _Atomic long       g_connection_capacity;
static _Atomic long long  g_listener_disabled_count;
static _Atomic long long  g_listener_disabled_ms;
static _Atomic long long  g_overload_responses;
static _Atomic long long  g_connection_resets;
static _Atomic long       g_buffer_bytes_current;
static _Atomic long       g_buffer_bytes_max;
static _Atomic long       g_backlog_depth;
static _Atomic long       g_backlog_depth_max;

/* Phase 3: TLS termination counters (cumulative). */
static _Atomic long long  g_tls_connections;
static _Atomic long long  g_tls_handshakes;
static _Atomic long long  g_tls_resumptions;
static _Atomic long long  g_tls_handshake_failures;

/* Phase 5: request-latency histogram and lifecycle gauges. */
#define METRICS_LAT_BUCKETS 13
static const long METRICS_LAT_US[METRICS_LAT_BUCKETS] = {
    1000, 2500, 5000, 10000, 25000, 50000, 100000,
    250000, 500000, 1000000, 2500000, 5000000, 10000000
};
static const char *const METRICS_LAT_LE[METRICS_LAT_BUCKETS] = {
    "0.001", "0.0025", "0.005", "0.01", "0.025", "0.05", "0.1",
    "0.25", "0.5", "1", "2.5", "5", "10"
};
static _Atomic long long  g_lat_buckets[METRICS_LAT_BUCKETS + 1]; /* +Inf */
static _Atomic long long  g_lat_sum_us;
static _Atomic long long  g_lat_count;
static _Atomic long long  g_start_time_epoch;
static _Atomic int        g_ready;

/* Phase 5: listener drops and accept errors by errno (cumulative). */
static _Atomic long long  g_listen_drops;
static _Atomic long long  g_accept_errors;
static _Atomic long long  g_accept_error_emfile;
static _Atomic long long  g_accept_error_enfile;
static _Atomic long long  g_accept_error_econnaborted;
static _Atomic long long  g_accept_error_other;

/* Optional cache-stats sampler invoked by the reporter while rendering. */
static _Atomic(MetricsCacheSampler) g_cache_sampler;

/* Per-loop event-loop counters, emitted as arrays in the snapshot. */
#define METRICS_MAX_EL_LOOPS 16
static _Atomic long long g_el_loop_wakeups[METRICS_MAX_EL_LOOPS];
static _Atomic long long g_el_loop_accepted[METRICS_MAX_EL_LOOPS];
static _Atomic int       g_el_loop_count;

/* Phase 3 event-loop counters (cumulative). */
static _Atomic long long g_el_wakeups;
static _Atomic long long g_el_readable_events;
static _Atomic long long g_el_writable_events;
static _Atomic long long g_el_eagain;
static _Atomic long long g_el_partial_writes;
static _Atomic long long g_el_deadline_closes;
static _Atomic long long g_el_pipeline_full;
static _Atomic long long g_el_output_drained;
static _Atomic long long g_el_connections_opened;
static _Atomic long long g_el_connections_closed;

/*
 * Response-class distribution. Each tracked status code gets a slot; the
 * trailing slot at index STATUS_BUCKETS collects every other status.
 */
#define STATUS_BUCKETS 7
static const int STATUS_CODES[STATUS_BUCKETS] = {200, 400, 404, 413, 431, 500, 501};
static _Atomic long long g_status[STATUS_BUCKETS + 1]; /* last slot: other */

/* --- Cumulative counter updates ----------------------------------------- */

/* Increment a cumulative counter with the relaxed ordering used throughout. */
static void bump(_Atomic long long *counter)
{
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

void metrics_connection_accepted(void)
{
    bump(&g_accepted_connections);
}

static void metrics_request_completed(void)
{
    bump(&g_completed_requests);
}

void metrics_response(int status)
{
    /* Every emitted response counts as a completion; whether it is also a
     * "failure" is decided by the caller (e.g. a 500 or a transport error),
     * because expected 4xx responses from the error-path scenarios must not
     * be reported as server failures. */
    metrics_request_completed();

    /* Map the status to its bucket, defaulting to the trailing "other" slot. */
    int slot = STATUS_BUCKETS; /* "other" */
    for (int i = 0; i < STATUS_BUCKETS; i++) {
        if (STATUS_CODES[i] == status) {
            slot = i;
            break;
        }
    }
    atomic_fetch_add_explicit(&g_status[slot], 1, memory_order_relaxed);
}

/* --- Active-connection gauge -------------------------------------------- */

void metrics_active_connection_dec(void)
{
    atomic_fetch_sub_explicit(&g_active_connections, 1, memory_order_relaxed);
}

long metrics_active_connection_count(void)
{
    return atomic_load_explicit(&g_active_connections, memory_order_relaxed);
}

/* --- Overload and limit counters ---------------------------------------- */

void metrics_admission_rejected_reason(AdmissionRejectReason reason)
{
    if ((int)reason < 0 || (int)reason >= ADMISSION_REJECT_REASON_COUNT)
        return;
    bump(&g_admission_rejected);
    atomic_fetch_add_explicit(&g_admission_rejected_by_reason[reason], 1,
                              memory_order_relaxed);
}

void metrics_header_timeout(void)     { bump(&g_header_timeout); }
void metrics_idle_timeout(void)       { bump(&g_idle_timeout); }
void metrics_write_timeout(void)      { bump(&g_write_timeout); }
void metrics_input_buffer_limit(void) { bump(&g_input_buffer_limit); }

void metrics_el_wakeup(void)             { bump(&g_el_wakeups); }
void metrics_el_readable_event(void)     { bump(&g_el_readable_events); }
void metrics_el_writable_event(void)     { bump(&g_el_writable_events); }
void metrics_el_eagain(void)             { bump(&g_el_eagain); }
void metrics_el_partial_write(void)      { bump(&g_el_partial_writes); }
void metrics_el_deadline_close(void)     { bump(&g_el_deadline_closes); }
void metrics_el_pipeline_full(void)      { bump(&g_el_pipeline_full); }
void metrics_el_output_drained(void)     { bump(&g_el_output_drained); }
void metrics_el_connection_opened(void)  { bump(&g_el_connections_opened); }
void metrics_el_connection_closed(void)  { bump(&g_el_connections_closed); }

void metrics_el_loop_count(int count)
{
    if (count < 0)
        count = 0;
    if (count > METRICS_MAX_EL_LOOPS)
        count = METRICS_MAX_EL_LOOPS;
    atomic_store_explicit(&g_el_loop_count, count, memory_order_relaxed);
}

void metrics_el_loop_wakeup(int loop_id)
{
    if (loop_id < 0 || loop_id >= METRICS_MAX_EL_LOOPS)
        return;
    atomic_fetch_add_explicit(&g_el_loop_wakeups[loop_id], 1,
                              memory_order_relaxed);
}

void metrics_el_loop_accepted(int loop_id)
{
    if (loop_id < 0 || loop_id >= METRICS_MAX_EL_LOOPS)
        return;
    atomic_fetch_add_explicit(&g_el_loop_accepted[loop_id], 1,
                              memory_order_relaxed);
}

/* --- Phase 4 saturate / backpressure ------------------------------------ */

void metrics_set_connection_capacity(long capacity)
{
    atomic_store_explicit(&g_connection_capacity, capacity, memory_order_relaxed);
}

int metrics_connection_admit(long capacity)
{
    /* Reserve a slot only if the process-wide gauge is below the cap. The CAS
     * loop makes this correct even when several event loops accept at once. */
    long current = atomic_load_explicit(&g_active_connections, memory_order_relaxed);
    while (current < capacity) {
        if (atomic_compare_exchange_weak_explicit(
                &g_active_connections, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed)) {
            /* Track the high-water mark exactly like metrics_active_connection_inc. */
            long observed = atomic_load_explicit(&g_active_connections_max,
                                                 memory_order_relaxed);
            while (current + 1 > observed &&
                   !atomic_compare_exchange_weak_explicit(
                       &g_active_connections_max, &observed, current + 1,
                       memory_order_relaxed, memory_order_relaxed)) {
            }
            return 1;
        }
    }
    return 0;
}

void metrics_listener_disabled(void)
{
    atomic_fetch_add_explicit(&g_listener_disabled_count, 1, memory_order_relaxed);
}

void metrics_listener_enabled(long disabled_ms)
{
    if (disabled_ms > 0) {
        atomic_fetch_add_explicit(&g_listener_disabled_ms, disabled_ms,
                                  memory_order_relaxed);
    }
}

void metrics_overload_response(void)
{
    bump(&g_overload_responses);
}

void metrics_connection_reset(void)
{
    bump(&g_connection_resets);
}

void metrics_buffer_leased(size_t bytes)
{
    long value = atomic_fetch_add_explicit(&g_buffer_bytes_current, (long)bytes,
                                           memory_order_relaxed) + (long)bytes;
    long observed = atomic_load_explicit(&g_buffer_bytes_max, memory_order_relaxed);
    while (value > observed &&
           !atomic_compare_exchange_weak_explicit(&g_buffer_bytes_max, &observed,
                                                  value, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

void metrics_buffer_returned(size_t bytes)
{
    atomic_fetch_sub_explicit(&g_buffer_bytes_current, (long)bytes,
                              memory_order_relaxed);
}

void metrics_backlog_depth(long depth)
{
    if (depth < 0)
        return;
    atomic_store_explicit(&g_backlog_depth, depth, memory_order_relaxed);
    long observed = atomic_load_explicit(&g_backlog_depth_max,
                                         memory_order_relaxed);
    while (depth > observed &&
           !atomic_compare_exchange_weak_explicit(&g_backlog_depth_max,
                                                  &observed, depth,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

/* --- Phase 3: TLS termination ------------------------------------------- */

void metrics_tls_connection(void)         { bump(&g_tls_connections); }
void metrics_tls_handshake(void)          { bump(&g_tls_handshakes); }
void metrics_tls_resumption(void)         { bump(&g_tls_resumptions); }
void metrics_tls_handshake_failure(void)  { bump(&g_tls_handshake_failures); }

/* --- Phase 5: listener drops and accept errors -------------------------- */

void metrics_listen_drops(void)
{
    bump(&g_listen_drops);
}

void metrics_accept_error(int errno_value)
{
    bump(&g_accept_errors);
    switch (errno_value) {
    case EMFILE:       bump(&g_accept_error_emfile);       break;
    case ENFILE:       bump(&g_accept_error_enfile);       break;
    case ECONNABORTED: bump(&g_accept_error_econnaborted); break;
    default:           bump(&g_accept_error_other);        break;
    }
}

/* --- Phase 5: latency histogram and lifecycle gauges -------------------- */

void metrics_observe_request_latency(long latency_us)
{
    if (latency_us < 0)
        latency_us = 0;

    int slot = METRICS_LAT_BUCKETS; /* +Inf */
    for (int i = 0; i < METRICS_LAT_BUCKETS; i++) {
        if (latency_us <= METRICS_LAT_US[i]) {
            slot = i;
            break;
        }
    }
    atomic_fetch_add_explicit(&g_lat_buckets[slot], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lat_sum_us, latency_us, memory_order_relaxed);
    bump(&g_lat_count);
}

void metrics_set_start_time(long long epoch_seconds)
{
    atomic_store_explicit(&g_start_time_epoch, epoch_seconds,
                          memory_order_relaxed);
}

void metrics_set_ready(int ready)
{
    atomic_store_explicit(&g_ready, ready ? 1 : 0, memory_order_relaxed);
}

int metrics_is_ready(void)
{
    return atomic_load_explicit(&g_ready, memory_order_relaxed);
}

/* --- Snapshot formatting ------------------------------------------------ */

/*
 * Append to `buf` at `off` using printf semantics, clamping safely when the
 * buffer is already full. Returns the new offset, which may exceed `cap` so the
 * caller can still report the untruncated length.
 */
static size_t appendf(char *buf, size_t cap, size_t off, const char *fmt, ...)
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

/* Read a counter with the relaxed ordering used by its updates. */
#define LOAD(counter) atomic_load_explicit(&(counter), memory_order_relaxed)

/*
 * Count the process's open file descriptors from /proc/self/fd. The server is
 * non-dumpable (PR_SET_DUMPABLE=0), so an external sampler cannot read
 * /proc/<pid>/fd; the process must report its own count. Returns -1 when procfs
 * is unavailable. The directory handle opened by opendir() is itself one of the
 * listed descriptors, so subtract it.
 */
static long count_open_fds(void)
{
#if defined(__linux__)
    DIR *dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;
    long count = 0;
    for (;;) {
        struct dirent *ent = readdir(dir);
        if (!ent)
            break;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        count++;
    }
    closedir(dir);
    return count > 0 ? count - 1 : count;
#else
    return -1;
#endif
}

/*
 * Serialize the counters into one line of JSON. The output is intentionally
 * flat and fixed-shape so the benchmark can parse it with the standard library
 * and so CSV columns line up across runs. `snprintf` returns the length the
 * buffer *would* need, which callers use to detect truncation.
 */
size_t metrics_snapshot(char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return 0;
    }
    /* Take one consistent-enough copy of the status buckets before printing. */
    long long status[STATUS_BUCKETS + 1];
    for (int i = 0; i <= STATUS_BUCKETS; i++) {
        status[i] = LOAD(g_status[i]);
    }

    size_t used = (size_t)snprintf(
        buf, cap,
        "{\"accepted_connections\":%lld,"
        "\"completed_requests\":%lld,"
        "\"status_200\":%lld,"
        "\"status_400\":%lld,"
        "\"status_404\":%lld,"
        "\"status_413\":%lld,"
        "\"status_431\":%lld,"
        "\"status_500\":%lld,"
        "\"status_501\":%lld,"
        "\"status_other\":%lld,"
        "\"active_connections\":%ld,"
        "\"active_connections_max\":%ld,"
        "\"admission_rejected\":%lld,"
        "\"header_timeout\":%lld,"
        "\"idle_timeout\":%lld,"
        "\"write_timeout\":%lld,"
        "\"input_buffer_limit\":%lld,"
        "\"el_wakeups\":%lld,"
        "\"el_readable_events\":%lld,"
        "\"el_writable_events\":%lld,"
        "\"el_eagain\":%lld,"
        "\"el_partial_writes\":%lld,"
        "\"el_deadline_closes\":%lld,"
        "\"el_pipeline_full\":%lld,"
        "\"el_output_drained\":%lld,"
        "\"el_connections_opened\":%lld,"
        "\"el_connections_closed\":%lld,"
        "\"connection_capacity\":%ld,"
        "\"listener_disabled_count\":%lld,"
        "\"listener_disabled_ms\":%lld,"
        "\"overload_responses\":%lld,"
        "\"connection_resets\":%lld,"
        "\"buffer_bytes_current\":%ld,"
        "\"buffer_bytes_max\":%ld,"
        "\"admission_rejected_capacity\":%lld,"
        "\"admission_rejected_table_full\":%lld,"
        "\"backlog_depth\":%ld,"
        "\"backlog_depth_max\":%ld,"
        "\"listen_drops\":%lld,"
        "\"accept_errors\":%lld,"
        "\"accept_error_emfile\":%lld,"
        "\"accept_error_enfile\":%lld,"
        "\"accept_error_econnaborted\":%lld,"
        "\"accept_error_other\":%lld,"
        "\"tls_connections\":%lld,"
        "\"tls_handshakes\":%lld,"
        "\"tls_resumptions\":%lld,"
        "\"tls_handshake_failures\":%lld,"
        "\"el_loops\":%d",
        LOAD(g_accepted_connections),
        LOAD(g_completed_requests),
        status[0], status[1], status[2], status[3], status[4], status[5],
        status[6], status[7],
        LOAD(g_active_connections),
        LOAD(g_active_connections_max),
        LOAD(g_admission_rejected),
        LOAD(g_header_timeout),
        LOAD(g_idle_timeout),
        LOAD(g_write_timeout),
        LOAD(g_input_buffer_limit),
        LOAD(g_el_wakeups),
        LOAD(g_el_readable_events),
        LOAD(g_el_writable_events),
        LOAD(g_el_eagain),
        LOAD(g_el_partial_writes),
        LOAD(g_el_deadline_closes),
        LOAD(g_el_pipeline_full),
        LOAD(g_el_output_drained),
        LOAD(g_el_connections_opened),
        LOAD(g_el_connections_closed),
        LOAD(g_connection_capacity),
        LOAD(g_listener_disabled_count),
        LOAD(g_listener_disabled_ms),
        LOAD(g_overload_responses),
        LOAD(g_connection_resets),
        LOAD(g_buffer_bytes_current),
        LOAD(g_buffer_bytes_max),
        LOAD(g_admission_rejected_by_reason[ADMISSION_REJECT_CAPACITY]),
        LOAD(g_admission_rejected_by_reason[ADMISSION_REJECT_TABLE_FULL]),
        LOAD(g_backlog_depth),
        LOAD(g_backlog_depth_max),
        LOAD(g_listen_drops),
        LOAD(g_accept_errors),
        LOAD(g_accept_error_emfile),
        LOAD(g_accept_error_enfile),
        LOAD(g_accept_error_econnaborted),
        LOAD(g_accept_error_other),
        LOAD(g_tls_connections),
        LOAD(g_tls_handshakes),
        LOAD(g_tls_resumptions),
        LOAD(g_tls_handshake_failures),
        LOAD(g_el_loop_count));

    int nloops = LOAD(g_el_loop_count);
    if (nloops < 0)
        nloops = 0;
    if (nloops > METRICS_MAX_EL_LOOPS)
        nloops = METRICS_MAX_EL_LOOPS;

    used = appendf(buf, cap, used, ",\"el_loop_wakeups\":[");
    for (int i = 0; i < nloops; i++) {
        used = appendf(buf, cap, used, "%s%lld", i ? "," : "",
                       LOAD(g_el_loop_wakeups[i]));
    }
    used = appendf(buf, cap, used, "],\"el_loop_accepted\":[");
    for (int i = 0; i < nloops; i++) {
        used = appendf(buf, cap, used, "%s%lld", i ? "," : "",
                       LOAD(g_el_loop_accepted[i]));
    }
    used = appendf(buf, cap, used, "]");

    MetricsCacheSampler sampler =
        atomic_load_explicit(&g_cache_sampler, memory_order_relaxed);
    long cache_bytes = 0, cache_entries = 0;
    if (sampler)
        sampler(&cache_bytes, &cache_entries);
    used = appendf(buf, cap, used,
                   ",\"cache_bytes\":%ld,\"cache_entries\":%ld"
                   ",\"open_fds\":%ld",
                   cache_bytes, cache_entries, count_open_fds());

    used = memory_profiler_append_json(buf, cap, used);
    used = appendf(buf, cap, used, "}");
    return used;
}

#undef LOAD

/* --- Phase 5: Prometheus exposition ------------------------------------- */

/*
 * Render the same lock-free counters in the Prometheus text exposition format
 * (version 0.0.4). The output is intentionally independent of the JSON
 * snapshot: scrapers get one metric per line with HELP/TYPE metadata while the
 * benchmark harness keeps its flat JSON. Callers bound the buffer; output past
 * `cap` is truncated rather than growing without limit.
 */
size_t metrics_prometheus(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;

    long long status[STATUS_BUCKETS + 1];
    for (int i = 0; i <= STATUS_BUCKETS; i++)
        status[i] = atomic_load_explicit(&g_status[i], memory_order_relaxed);

    long long lat[METRICS_LAT_BUCKETS + 1];
    for (int i = 0; i <= METRICS_LAT_BUCKETS; i++)
        lat[i] = atomic_load_explicit(&g_lat_buckets[i], memory_order_relaxed);

    long long start_epoch =
        atomic_load_explicit(&g_start_time_epoch, memory_order_relaxed);
    long long uptime = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        uptime = (long long)now.tv_sec - start_epoch;
        if (uptime < 0)
            uptime = 0;
    }

    long cache_bytes = 0, cache_entries = 0;
    MetricsCacheSampler sampler =
        atomic_load_explicit(&g_cache_sampler, memory_order_relaxed);
    if (sampler)
        sampler(&cache_bytes, &cache_entries);

    long rss_kb = 0, vm_kb = 0;
    memory_profiler_get(&rss_kb, &vm_kb);

#define PROM(...) used = appendf(buf, cap, used, __VA_ARGS__)
    size_t used = 0;

    PROM("# HELP simplehttp_ready 1 when the server is accepting connections.\n"
         "# TYPE simplehttp_ready gauge\n"
         "simplehttp_ready %d\n"
         "# HELP simplehttp_start_time_seconds Process start time since unix "
         "epoch.\n"
         "# TYPE simplehttp_start_time_seconds gauge\n"
         "simplehttp_start_time_seconds %lld\n"
         "# HELP simplehttp_uptime_seconds Seconds since process start.\n"
         "# TYPE simplehttp_uptime_seconds gauge\n"
         "simplehttp_uptime_seconds %lld\n",
         atomic_load_explicit(&g_ready, memory_order_relaxed), start_epoch,
         uptime);

    PROM("# HELP simplehttp_requests_total Completed responses.\n"
         "# TYPE simplehttp_requests_total counter\n"
         "simplehttp_requests_total %lld\n"
         "# HELP simplehttp_connections_accepted_total Accepted TCP "
         "connections.\n"
         "# TYPE simplehttp_connections_accepted_total counter\n"
         "simplehttp_connections_accepted_total %lld\n"
         "# HELP simplehttp_connections_closed_total Closed connections.\n"
         "# TYPE simplehttp_connections_closed_total counter\n"
         "simplehttp_connections_closed_total %lld\n"
         "# HELP simplehttp_active_connections Live connections.\n"
         "# TYPE simplehttp_active_connections gauge\n"
         "simplehttp_active_connections %ld\n"
         "# HELP simplehttp_active_connections_max High-water live "
         "connections.\n"
         "# TYPE simplehttp_active_connections_max gauge\n"
         "simplehttp_active_connections_max %ld\n"
         "# HELP simplehttp_connection_capacity Effective connection cap.\n"
         "# TYPE simplehttp_connection_capacity gauge\n"
         "simplehttp_connection_capacity %ld\n",
         atomic_load_explicit(&g_completed_requests, memory_order_relaxed),
         atomic_load_explicit(&g_accepted_connections, memory_order_relaxed),
         atomic_load_explicit(&g_el_connections_closed, memory_order_relaxed),
         atomic_load_explicit(&g_active_connections, memory_order_relaxed),
         atomic_load_explicit(&g_active_connections_max, memory_order_relaxed),
         atomic_load_explicit(&g_connection_capacity, memory_order_relaxed));

    PROM("# HELP simplehttp_responses_total Responses by status class.\n"
         "# TYPE simplehttp_responses_total counter\n");
    for (int i = 0; i < STATUS_BUCKETS; i++) {
        PROM("simplehttp_responses_total{status=\"%d\"} %lld\n",
             STATUS_CODES[i], status[i]);
    }
    PROM("simplehttp_responses_total{status=\"other\"} %lld\n", status[STATUS_BUCKETS]);

    /* Prometheus histograms are cumulative: each `le` bucket counts every
     * observation at or below its bound. */
    PROM("# HELP simplehttp_request_duration_seconds End-to-end request "
         "latency in seconds.\n"
         "# TYPE simplehttp_request_duration_seconds histogram\n");
    long long cumulative = 0;
    for (int i = 0; i < METRICS_LAT_BUCKETS; i++) {
        cumulative += lat[i];
        PROM("simplehttp_request_duration_seconds_bucket{le=\"%s\"} %lld\n",
             METRICS_LAT_LE[i], cumulative);
    }
    cumulative += lat[METRICS_LAT_BUCKETS];
    PROM("simplehttp_request_duration_seconds_bucket{le=\"+Inf\"} %lld\n"
         "simplehttp_request_duration_seconds_sum %lld\n"
         "simplehttp_request_duration_seconds_count %lld\n",
         cumulative,
         atomic_load_explicit(&g_lat_sum_us, memory_order_relaxed),
         atomic_load_explicit(&g_lat_count, memory_order_relaxed));

    PROM("# HELP simplehttp_admission_rejected_total Rejected new connections.\n"
         "# TYPE simplehttp_admission_rejected_total counter\n"
         "simplehttp_admission_rejected_total %lld\n"
         "# HELP simplehttp_overload_responses_total 503 overload responses "
         "sent.\n"
         "# TYPE simplehttp_overload_responses_total counter\n"
         "simplehttp_overload_responses_total %lld\n"
         "# HELP simplehttp_header_timeouts_total Header read timeouts.\n"
         "# TYPE simplehttp_header_timeouts_total counter\n"
         "simplehttp_header_timeouts_total %lld\n"
         "# HELP simplehttp_idle_timeouts_total Keep-alive idle timeouts.\n"
         "# TYPE simplehttp_idle_timeouts_total counter\n"
         "simplehttp_idle_timeouts_total %lld\n"
         "# HELP simplehttp_write_timeouts_total Write timeouts.\n"
         "# TYPE simplehttp_write_timeouts_total counter\n"
         "simplehttp_write_timeouts_total %lld\n"
         "# HELP simplehttp_input_buffer_limit_total Input buffer limit "
         "closes.\n"
         "# TYPE simplehttp_input_buffer_limit_total counter\n"
         "simplehttp_input_buffer_limit_total %lld\n"
         "# HELP simplehttp_listen_drops_total Kernel accept-queue drops.\n"
         "# TYPE simplehttp_listen_drops_total counter\n"
         "simplehttp_listen_drops_total %lld\n"
         "# HELP simplehttp_accept_errors_total accept() failures.\n"
         "# TYPE simplehttp_accept_errors_total counter\n"
         "simplehttp_accept_errors_total %lld\n",
         atomic_load_explicit(&g_admission_rejected, memory_order_relaxed),
         atomic_load_explicit(&g_overload_responses, memory_order_relaxed),
         atomic_load_explicit(&g_header_timeout, memory_order_relaxed),
         atomic_load_explicit(&g_idle_timeout, memory_order_relaxed),
         atomic_load_explicit(&g_write_timeout, memory_order_relaxed),
         atomic_load_explicit(&g_input_buffer_limit, memory_order_relaxed),
         atomic_load_explicit(&g_listen_drops, memory_order_relaxed),
         atomic_load_explicit(&g_accept_errors, memory_order_relaxed));

    PROM("# HELP simplehttp_tls_connections_total TLS connections accepted.\n"
         "# TYPE simplehttp_tls_connections_total counter\n"
         "simplehttp_tls_connections_total %lld\n"
         "# HELP simplehttp_tls_handshakes_total Completed TLS handshakes.\n"
         "# TYPE simplehttp_tls_handshakes_total counter\n"
         "simplehttp_tls_handshakes_total %lld\n"
         "# HELP simplehttp_tls_resumptions_total Resumed TLS handshakes.\n"
         "# TYPE simplehttp_tls_resumptions_total counter\n"
         "simplehttp_tls_resumptions_total %lld\n"
         "# HELP simplehttp_tls_handshake_failures_total Failed TLS "
         "handshakes.\n"
         "# TYPE simplehttp_tls_handshake_failures_total counter\n"
         "simplehttp_tls_handshake_failures_total %lld\n",
         atomic_load_explicit(&g_tls_connections, memory_order_relaxed),
         atomic_load_explicit(&g_tls_handshakes, memory_order_relaxed),
         atomic_load_explicit(&g_tls_resumptions, memory_order_relaxed),
         atomic_load_explicit(&g_tls_handshake_failures, memory_order_relaxed));

    PROM("# HELP simplehttp_cache_bytes Bytes held by the representation "
         "cache.\n"
         "# TYPE simplehttp_cache_bytes gauge\n"
         "simplehttp_cache_bytes %ld\n"
         "# HELP simplehttp_cache_entries Entries in the representation cache.\n"
         "# TYPE simplehttp_cache_entries gauge\n"
         "simplehttp_cache_entries %ld\n"
         "# HELP simplehttp_open_fds Open file descriptors held by the server.\n"
         "# TYPE simplehttp_open_fds gauge\n"
         "simplehttp_open_fds %ld\n"
         "# HELP simplehttp_resident_memory_bytes Resident set size.\n"
         "# TYPE simplehttp_resident_memory_bytes gauge\n"
         "simplehttp_resident_memory_bytes %lld\n"
         "# HELP simplehttp_virtual_memory_bytes Virtual memory size.\n"
         "# TYPE simplehttp_virtual_memory_bytes gauge\n"
         "simplehttp_virtual_memory_bytes %lld\n",
         cache_bytes, cache_entries, count_open_fds(),
         (long long)rss_kb * 1024, (long long)vm_kb * 1024);

#undef PROM
    return used;
}

/* --- Reporter thread ---------------------------------------------------- */

static _Atomic int g_reporter_stop;    /* set by stop() to end the loop     */
static _Atomic int g_reporter_running; /* nonzero while the thread is alive */
static char g_reporter_path[4096];     /* destination file chosen by caller */
static int g_reporter_interval_ms = 1000;

/*
 * Write one snapshot. We write to "<path>.tmp" and rename it over the target
 * so a concurrent reader (the benchmark) never observes a half-written file;
 * rename(2) is atomic within a filesystem.
 */
static void write_snapshot(const char *path)
{
    /* Refresh the memory sample on this (reporter) thread so the data path
     * never touches procfs or the allocator accounting. */
    memory_profiler_sample();

    char json[8192];
    metrics_snapshot(json, sizeof(json));

    char tmp[sizeof(g_reporter_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return; /* unwritable path: keep serving, just skip this snapshot */
    }
    fprintf(fp, "%s\n", json);
    fflush(fp);
    fclose(fp);
    if (rename(tmp, path) != 0) {
        remove(tmp); /* do not leave stray temp files behind */
    }
}

static void *reporter_thread(void *arg)
{
    (void)arg;
    /* Publish immediately, then at a fixed interval, so even a very short
     * benchmark run produces at least one useful snapshot. */
    while (!atomic_load_explicit(&g_reporter_stop, memory_order_relaxed)) {
        write_snapshot(g_reporter_path);
        struct timespec delay;
        delay.tv_sec = g_reporter_interval_ms / 1000;
        delay.tv_nsec = (long)(g_reporter_interval_ms % 1000) * 1000000L;
        nanosleep(&delay, NULL);
    }
    /* Final snapshot so the last counter updates are visible to the reader. */
    write_snapshot(g_reporter_path);
    atomic_store_explicit(&g_reporter_running, 0, memory_order_relaxed);
    return NULL;
}

void metrics_reporter_start(const char *path, int interval_ms)
{
    /* Ignore empty paths and repeated starts; one reporter per process. */
    if (!path || !*path || atomic_load_explicit(&g_reporter_running, memory_order_relaxed)) {
        return;
    }
    snprintf(g_reporter_path, sizeof(g_reporter_path), "%s", path);
    g_reporter_interval_ms = interval_ms > 0 ? interval_ms : 1000;
    atomic_store_explicit(&g_reporter_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_reporter_running, 1, memory_order_relaxed);

    /* Detached: the reporter cleans itself up and is stopped explicitly by
     * metrics_reporter_stop() during shutdown. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    if (pthread_create(&tid, &attr, reporter_thread, NULL) != 0) {
        atomic_store_explicit(&g_reporter_running, 0, memory_order_relaxed);
    }
    pthread_attr_destroy(&attr);
}

void metrics_reporter_stop(void)
{
    atomic_store_explicit(&g_reporter_stop, 1, memory_order_relaxed);
}

void metrics_set_cache_sampler(MetricsCacheSampler fn)
{
    atomic_store_explicit(&g_cache_sampler, fn, memory_order_relaxed);
}
