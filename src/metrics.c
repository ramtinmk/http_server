#include "metrics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * All counters use C11 relaxed atomics. The values are independent statistics
 * where a slightly stale read is acceptable, so we only need atomicity (to
 * avoid torn updates across the 16 worker threads) and not ordering relative
 * to other memory. This keeps the instrumentation invisible in profiles.
 */

/* Cumulative totals (monotonic; only ever incremented). */
static _Atomic long long g_accepted_connections; /* connections accepted   */
static _Atomic long long g_completed_requests;   /* responses sent         */
static _Atomic long long g_request_failures;     /* failed before response */
static _Atomic long long g_rejected_tasks;       /* dropped due to full q  */

/* Gauges: `_depth`/`_workers` are the live values, `_max` is the high-water
 * mark observed since process start. The benchmark cares about the maximum
 * because the queue is usually drained between sampling points. */
static _Atomic long g_queue_depth;
static _Atomic long g_queue_depth_max;
static _Atomic long g_active_workers;
static _Atomic long g_active_workers_max;

/* Active-connection gauge (live connections, not cumulative). */
static _Atomic long g_active_connections;
static _Atomic long g_active_connections_max;

/* Overload and limit counters (cumulative). */
static _Atomic long long g_admission_rejected;
static _Atomic long long g_buffer_pool_exhausted;
static _Atomic long long g_header_timeout;
static _Atomic long long g_idle_timeout;
static _Atomic long long g_write_timeout;
static _Atomic long long g_input_buffer_limit;

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
 * extra slot at the end (STATUS_BUCKETS) collects everything else. The
 * sentinel 0 in STATUS_CODES is never matched because real status codes are
 * >= 100, so it simply marks the end of the explicit list.
 */
#define STATUS_BUCKETS 8
static const int STATUS_CODES[STATUS_BUCKETS] = {200, 400, 404, 413, 431, 500, 501, 0};
static _Atomic long long g_status[STATUS_BUCKETS + 1]; /* last slot: other */
static _Atomic long long g_status_total;

/* --- Cumulative counter updates ----------------------------------------- */

void metrics_connection_accepted(void)
{
    atomic_fetch_add_explicit(&g_accepted_connections, 1, memory_order_relaxed);
}

void metrics_request_completed(void)
{
    atomic_fetch_add_explicit(&g_completed_requests, 1, memory_order_relaxed);
}

void metrics_request_failed(void)
{
    atomic_fetch_add_explicit(&g_request_failures, 1, memory_order_relaxed);
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
    atomic_fetch_add_explicit(&g_status_total, 1, memory_order_relaxed);
}

void metrics_task_rejected(void)
{
    atomic_fetch_add_explicit(&g_rejected_tasks, 1, memory_order_relaxed);
}

/* --- Gauge helpers ------------------------------------------------------ */

/*
 * Increment a gauge and atomically raise its high-water mark. The CAS loop is
 * only entered when the new value beats the currently observed maximum, so in
 * the common case (steady state) this is a single load plus a compare.
 */
static void gauge_inc(_Atomic long *gauge, _Atomic long *high)
{
    long value = atomic_fetch_add_explicit(gauge, 1, memory_order_relaxed) + 1;
    long observed = atomic_load_explicit(high, memory_order_relaxed);
    while (value > observed &&
           !atomic_compare_exchange_weak_explicit(high, &observed, value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
        /* On failure compare_exchange refreshes `observed`; loop if still behind. */
    }
}

static void gauge_dec(_Atomic long *gauge)
{
    atomic_fetch_sub_explicit(gauge, 1, memory_order_relaxed);
}

void metrics_queue_enqueued(void)
{
    gauge_inc(&g_queue_depth, &g_queue_depth_max);
}

void metrics_queue_dequeued(void)
{
    gauge_dec(&g_queue_depth);
}

void metrics_worker_busy(void)
{
    gauge_inc(&g_active_workers, &g_active_workers_max);
}

void metrics_worker_idle(void)
{
    gauge_dec(&g_active_workers);
}

/* --- Active-connection gauge -------------------------------------------- */

void metrics_active_connection_inc(void)
{
    gauge_inc(&g_active_connections, &g_active_connections_max);
}

void metrics_active_connection_dec(void)
{
    gauge_dec(&g_active_connections);
}

long metrics_active_connection_count(void)
{
    return atomic_load_explicit(&g_active_connections, memory_order_relaxed);
}

/* --- Overload and limit counters ---------------------------------------- */

void metrics_admission_rejected(void)
{
    atomic_fetch_add_explicit(&g_admission_rejected, 1, memory_order_relaxed);
}

void metrics_buffer_pool_exhausted(void)
{
    atomic_fetch_add_explicit(&g_buffer_pool_exhausted, 1, memory_order_relaxed);
}

void metrics_header_timeout(void)
{
    atomic_fetch_add_explicit(&g_header_timeout, 1, memory_order_relaxed);
}

void metrics_idle_timeout(void)
{
    atomic_fetch_add_explicit(&g_idle_timeout, 1, memory_order_relaxed);
}

void metrics_write_timeout(void)
{
    atomic_fetch_add_explicit(&g_write_timeout, 1, memory_order_relaxed);
}

void metrics_input_buffer_limit(void)
{
    atomic_fetch_add_explicit(&g_input_buffer_limit, 1, memory_order_relaxed);
}

void metrics_el_wakeup(void)
{
    atomic_fetch_add_explicit(&g_el_wakeups, 1, memory_order_relaxed);
}
void metrics_el_readable_event(void)
{
    atomic_fetch_add_explicit(&g_el_readable_events, 1, memory_order_relaxed);
}
void metrics_el_writable_event(void)
{
    atomic_fetch_add_explicit(&g_el_writable_events, 1, memory_order_relaxed);
}
void metrics_el_eagain(void)
{
    atomic_fetch_add_explicit(&g_el_eagain, 1, memory_order_relaxed);
}
void metrics_el_partial_write(void)
{
    atomic_fetch_add_explicit(&g_el_partial_writes, 1, memory_order_relaxed);
}
void metrics_el_deadline_close(void)
{
    atomic_fetch_add_explicit(&g_el_deadline_closes, 1, memory_order_relaxed);
}
void metrics_el_pipeline_full(void)
{
    atomic_fetch_add_explicit(&g_el_pipeline_full, 1, memory_order_relaxed);
}
void metrics_el_output_drained(void)
{
    atomic_fetch_add_explicit(&g_el_output_drained, 1, memory_order_relaxed);
}
void metrics_el_connection_opened(void)
{
    atomic_fetch_add_explicit(&g_el_connections_opened, 1, memory_order_relaxed);
}
void metrics_el_connection_closed(void)
{
    atomic_fetch_add_explicit(&g_el_connections_closed, 1, memory_order_relaxed);
}

/* --- Snapshot formatting ------------------------------------------------ */

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
        status[i] = atomic_load_explicit(&g_status[i], memory_order_relaxed);
    }

    size_t used = (size_t)snprintf(
        buf, cap,
        "{\"accepted_connections\":%lld,"
        "\"completed_requests\":%lld,"
        "\"request_failures\":%lld,"
        "\"rejected_tasks\":%lld,"
        "\"queue_depth\":%ld,"
        "\"queue_depth_max\":%ld,"
        "\"active_workers\":%ld,"
        "\"active_workers_max\":%ld,"
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
        "\"buffer_pool_exhausted\":%lld,"
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
        "\"el_connections_closed\":%lld}",
        atomic_load_explicit(&g_accepted_connections, memory_order_relaxed),
        atomic_load_explicit(&g_completed_requests, memory_order_relaxed),
        atomic_load_explicit(&g_request_failures, memory_order_relaxed),
        atomic_load_explicit(&g_rejected_tasks, memory_order_relaxed),
        atomic_load_explicit(&g_queue_depth, memory_order_relaxed),
        atomic_load_explicit(&g_queue_depth_max, memory_order_relaxed),
        atomic_load_explicit(&g_active_workers, memory_order_relaxed),
        atomic_load_explicit(&g_active_workers_max, memory_order_relaxed),
        status[0], status[1], status[2], status[3], status[4], status[5],
        status[6], status[7],
        atomic_load_explicit(&g_active_connections, memory_order_relaxed),
        atomic_load_explicit(&g_active_connections_max, memory_order_relaxed),
        atomic_load_explicit(&g_admission_rejected, memory_order_relaxed),
        atomic_load_explicit(&g_buffer_pool_exhausted, memory_order_relaxed),
        atomic_load_explicit(&g_header_timeout, memory_order_relaxed),
        atomic_load_explicit(&g_idle_timeout, memory_order_relaxed),
        atomic_load_explicit(&g_write_timeout, memory_order_relaxed),
        atomic_load_explicit(&g_input_buffer_limit, memory_order_relaxed),
        atomic_load_explicit(&g_el_wakeups,            memory_order_relaxed),
        atomic_load_explicit(&g_el_readable_events,    memory_order_relaxed),
        atomic_load_explicit(&g_el_writable_events,    memory_order_relaxed),
        atomic_load_explicit(&g_el_eagain,             memory_order_relaxed),
        atomic_load_explicit(&g_el_partial_writes,     memory_order_relaxed),
        atomic_load_explicit(&g_el_deadline_closes,    memory_order_relaxed),
        atomic_load_explicit(&g_el_pipeline_full,      memory_order_relaxed),
        atomic_load_explicit(&g_el_output_drained,     memory_order_relaxed),
        atomic_load_explicit(&g_el_connections_opened, memory_order_relaxed),
        atomic_load_explicit(&g_el_connections_closed, memory_order_relaxed));
    return used;
}

/* --- Reporter thread ---------------------------------------------------- */

static pthread_t g_reporter_thread;
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
    char json[4096];
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
    if (pthread_create(&g_reporter_thread, &attr, reporter_thread, NULL) != 0) {
        atomic_store_explicit(&g_reporter_running, 0, memory_order_relaxed);
    }
    pthread_attr_destroy(&attr);
}

void metrics_reporter_stop(void)
{
    atomic_store_explicit(&g_reporter_stop, 1, memory_order_relaxed);
}

int metrics_reporter_active(void)
{
    return atomic_load_explicit(&g_reporter_running, memory_order_relaxed);
}
