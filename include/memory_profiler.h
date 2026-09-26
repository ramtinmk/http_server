#ifndef MEMORY_PROFILER_H
#define MEMORY_PROFILER_H

#include <stddef.h>

/*
 * Runtime memory profiler.
 *
 * Samples the server process's own memory from the inside so a metrics or soak
 * run can see resident / proportional / heap / mmap usage without depending on
 * an external VmRSS scrape. Sampling is performed by the metrics reporter
 * thread and never by the event-loop data path.
 *
 * Each data source is Linux and best-effort:
 *   - /proc/self/statm       : virtual and resident set size (pages)
 *   - /proc/self/smaps_rollup: proportional set size (Pss, kB) and
 *                              Private_Dirty (anonymous resident kB)
 *   - mallinfo2()            : glibc heap and mmap bytes
 *
 * A source that is unavailable (non-Linux, restricted procfs, or an older
 * glibc) contributes zero and is reflected by memory_sample_ok rather than
 * being silently reported as real usage.
 */

/*
 * Refresh the cached sample and update the high-water marks. Called by the
 * metrics reporter thread at snapshot cadence; safe to call from one thread.
 */
void memory_profiler_sample(void);

/*
 * Append the profiler fields as JSON object members (leading comma included)
 * to `buf` at `off`, rendering the last cached sample. Returns the new offset
 * with snprintf truncation semantics, matching metrics.c's internal appendf.
 */
size_t memory_profiler_append_json(char *buf, size_t cap, size_t off);

#endif /* MEMORY_PROFILER_H */
